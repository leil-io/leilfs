/*
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <vector>

#include "chunkserver/chunkserver_entry.h"
#include "chunkserver/bgjobs.h"

inline std::atomic<uint16_t> gMaxParallelHddReadJobsPerCsEntry;
inline std::atomic<uint16_t> gMaxBlocksPerHddReadJob;
inline std::atomic<uint16_t> gMaxBlocksPerHddWriteJob;

class NetworkWorkerThread {
public:
	static constexpr uint32_t kDefaultNumberOfNetworkWorkers = 4;
	static constexpr uint32_t kDefaultNumberOfHddWorkersPerNetworkWorker = 16;
	static constexpr uint32_t kDefaultMaxBackgroundJobsPerNetworkWorker = 4000;

	static constexpr uint16_t kDefaultMaxParallelHddReadJobsPerCsEntry = 16;
	static constexpr uint16_t kDefaultMaxBlocksPerHddReadJob = 8;

	static constexpr uint16_t kDefaultMaxBlocksPerHddWriteJob = 16;
	static constexpr uint16_t kMinBlocksPerHddWriteJob = 1;
	static constexpr uint16_t kMaxBlocksPerHddWriteJob = 64;

	NetworkWorkerThread(uint32_t id, uint32_t nrOfBgjobsWorkers, uint32_t bgjobsCount);
	NetworkWorkerThread(const NetworkWorkerThread&) = delete;

	// main loop
	void operator()();
	void askForTermination();
	void addConnection(int newSocketFD);

	JobPool *backgroundJobPool() {
		return bgJobPool_.get();
	}

private:
	void preparePollFds();
	void servePoll() ;
	void terminate();

	/// Human readable name for the thread
	std::string name_;

	std::atomic<bool> doTerminate;
	std::mutex csservheadLock;
	std::list<ChunkserverEntry> csservEntries;

	std::unique_ptr<JobPool> bgJobPool_;
	int bgJobPoolWakeUpFd_;
	static const uint32_t JOB_FD_PDESC_POS = 1;
	std::vector<struct pollfd> pdesc;
	int notify_pipe[2];
};

