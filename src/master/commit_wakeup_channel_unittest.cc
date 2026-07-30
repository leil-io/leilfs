/*
   Copyright 2026      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include "master/commit_wakeup_channel.h"

namespace {

int openEventFd() {
	int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (fd < 0) { throw std::runtime_error("eventfd() failed in test setup"); }
	return fd;
}

}  // namespace

TEST(CommitWakeupChannelTests, WakeupPollDescPollServeDrainsFd) {
	CommitWakeupChannel channel;
	int fd = openEventFd();
	channel.arm(fd);

	channel.wakeup();

	std::vector<pollfd> pdesc;
	channel.pollDesc(pdesc);
	ASSERT_EQ(1u, pdesc.size());
	EXPECT_EQ(fd, pdesc[0].fd);

	pdesc[0].revents = POLLIN;
	channel.pollServe(pdesc);

	uint64_t drained = 0;
	EXPECT_EQ(-1, read(fd, &drained, sizeof(drained)));
	EXPECT_EQ(EAGAIN, errno);

	channel.retire();
}

// Stresses wakeup() (simulating the FDB network thread) against repeated
// retire()/arm() cycles (simulating shutdown racing an in-flight commit).
// This proves wakeup() and retire() never touch fd_ concurrently (no TSAN
// report), which is what the mutex is for. It does not independently prove
// fd-reuse safety: TSAN instruments the C++ memory model, not OS fd lifetime,
// so a hypothetical wakeup() that copied fd_ out and wrote after unlocking
// would still pass this test clean. That guarantee instead comes from
// wakeup()'s write() sitting fully inside its own critical section, and
// retire()'s close() sitting fully inside its critical section too, so
// wakeup() can never observe a half-invalidated fd_.
TEST(CommitWakeupChannelTests, WakeupDoesNotRaceRetire) {
	CommitWakeupChannel channel;
	channel.arm(openEventFd());

	std::atomic<bool> stop{false};
	std::thread wakeupThread([&channel, &stop]() {
		while (!stop.load(std::memory_order_relaxed)) { channel.wakeup(); }
	});

	constexpr int kCycles = 2000;
	for (int i = 0; i < kCycles; ++i) {
		channel.retire();
		channel.arm(openEventFd());
	}

	stop.store(true, std::memory_order_relaxed);
	wakeupThread.join();
	channel.retire();

	// Retired state must be terminal and harmless to poke further: no fd left
	// to watch, and a lagging wakeup() (or a repeated retire(), as shutdown
	// could plausibly do) must not resurrect it.
	std::vector<pollfd> pdescAfterRetire;
	channel.pollDesc(pdescAfterRetire);
	EXPECT_TRUE(pdescAfterRetire.empty());

	channel.wakeup();
	channel.retire();
	channel.pollDesc(pdescAfterRetire);
	EXPECT_TRUE(pdescAfterRetire.empty());
}
