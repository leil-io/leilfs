/*
   Copyright 2026 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <gtest/gtest.h>
#include <poll.h>
#include <chrono>
#include <cstdint>
#include <thread>

#include "chunkserver/network_worker_thread.h"

// The main thread inspects the worker's job pool in updateAndCheckTerminationStatus()
// before publishing canTerminate_, and the worker then destroys the pool in terminate().
// Delaying the worker's start, without synchronizing with it, completes the check
// before the worker first takes csservheadLock: the interleaving behind the
// SingleMachineTests.test_helgrind_basic failures. Run under Helgrind to verify that
// the lock, not only the atomic flag, orders the teardown after the check.
TEST(NetworkWorkerThreadTests, TerminationReleasesJobPoolAfterStatusCheck) {
	constexpr uint32_t kIterations = 3;
	constexpr uint32_t kJobPoolWorkers = 1;
	constexpr uint32_t kMaxJobs = 16;
	constexpr auto kJobPoolIdleDelay = std::chrono::milliseconds(100);
	constexpr auto kWorkerStartDelay = std::chrono::milliseconds(100);

	for (uint32_t iteration = 0; iteration < kIterations; ++iteration) {
		NetworkWorkerThread worker(iteration, kJobPoolWorkers, kMaxJobs);
		// A serving worker synchronizes with its pool's idle job threads through the job
		// queue. Do the same before starting it, leaving only the termination unordered.
		std::this_thread::sleep_for(kJobPoolIdleDelay);
		EXPECT_EQ(worker.backgroundJobPool()->getJobCount(), 0U);
		worker.askForTermination();
		std::thread workerThread([&worker, kWorkerStartDelay] {
			std::this_thread::sleep_for(kWorkerStartDelay);
			worker();
		});

		while (!worker.updateAndCheckTerminationStatus()) { std::this_thread::yield(); }

		workerThread.join();
		EXPECT_EQ(worker.backgroundJobPool(), nullptr);
	}
}
