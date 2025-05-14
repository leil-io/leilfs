/*
   Copyright 2025 Leil Storage OÜ

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

#include "chunkserver/bgjobs.h"
#include "errors/saunafs_error_codes.h"

#include <sys/poll.h>
#include <thread>

#include "gtest/gtest.h"

void mockJobCallback(uint8_t  /*status*/, void *extra) {
	auto *counter = static_cast<std::atomic<int> *>(extra);
	counter->fetch_add(1);
}

constexpr uint32_t kWaitTimeMs = 100;

// Test fixture for JobPool tests
class JobPoolTest : public ::testing::Test {
private:
	void servePoll() {
		while (!terminate) {
			int ret = poll(&wakeupDescPollFd, 1, kWaitTimeMs);
			if (ret > 0) {
				if (wakeupDescPollFd.revents & POLLIN) {
					jobPool->checkJobs();
				}
			}
		}
	}

protected:
	void SetUp() override {
		wakeupDesc = 0;
		jobPool = std::make_unique<JobPool>("TestPool", 4, 10, &wakeupDesc);
		counter.store(0);
		counter2.store(0);
		wakeupDescPollFd = {wakeupDesc, POLLIN, 0};
		startServePoll();
	}

	void TearDown() override {
		stopServePoll();
		jobPool.reset();
	}

	void startServePoll() {
		terminate = false;
		servePollThread = std::thread([this]() { servePoll(); });
	}

	void stopServePoll() {
		std::unique_lock lock(mutex_);
		terminate = true;
		lock.unlock();

		if (servePollThread.joinable()) { servePollThread.join(); }
	}

	JobPool::ProcessJobCallback mockProcessJob = []() -> uint8_t {
		return 0;  // Return success status
	};

	std::unique_ptr<JobPool> jobPool;
	int wakeupDesc;
	std::atomic<int> counter;
	std::atomic<int> counter2;
	struct pollfd wakeupDescPollFd;
	std::thread servePollThread;
	bool terminate;
	std::mutex mutex_;
};

// Test job addition
TEST_F(JobPoolTest, AddJob) {
	jobPool->addJob(JobPool::ChunkOperation::Read, mockJobCallback, &counter, mockProcessJob);
	EXPECT_EQ(jobPool->getJobCount(), 1U);
}

// Test job processing
TEST_F(JobPoolTest, ProcessJob) {
	jobPool->addJob(JobPool::ChunkOperation::Read, mockJobCallback, &counter, mockProcessJob);

	// Wait for the job to be processed
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));

	EXPECT_EQ(counter.load(), 1);
}

// Test job disabling
TEST_F(JobPoolTest, DisableJob) {
	JobPool::JobCallback mockedJobCallback = [](uint8_t status, void *extra) -> void {
		auto *counter = static_cast<std::atomic<int> *>(extra);
		counter->store(status);
	};

	uint32_t jobId =
	    jobPool->addJob(JobPool::ChunkOperation::Read, mockedJobCallback, &counter, mockProcessJob);
	jobPool->disableJob(jobId);

	// Wait for the job to be processed
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));

	EXPECT_EQ(counter.load(), SAUNAFS_ERROR_NOTDONE);
}

// Test changing callbacks
TEST_F(JobPoolTest, ChangeCallback) {
	uint32_t jobId =
	    jobPool->addJob(JobPool::ChunkOperation::Read, mockJobCallback, &counter, mockProcessJob);
	jobPool->changeCallback(jobId, mockJobCallback, &counter2);

	// Wait for the job to be processed
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));

	EXPECT_EQ(counter.load(), 0);
	EXPECT_EQ(counter2.load(), 1);
}

// Test job status handling
TEST_F(JobPoolTest, JobStatusHandling) {
	// Stop the servePoll thread to disable automatic job dispatching
	stopServePoll();

	jobPool->addJob(JobPool::ChunkOperation::Read, mockJobCallback, &counter, mockProcessJob);

	// Wait for the job to be processed
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));

	jobPool->checkJobs();
	EXPECT_EQ(counter.load(), 1);
}
