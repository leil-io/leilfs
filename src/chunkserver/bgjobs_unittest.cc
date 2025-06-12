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
	void servePoll(uint32_t listenerId) {
		struct pollfd wakeupDescPollFd = {wakeupDescVec[listenerId], POLLIN, 0};
		while (!terminate) {
			int ret = poll(&wakeupDescPollFd, 1, kWaitTimeMs);
			if (ret > 0) {
				if (wakeupDescPollFd.revents & POLLIN) {
					processingCount[listenerId].fetch_add(1);
					jobPool->processCompletedJobs(listenerId);
				}
			}
		}
	}

protected:
	void SetUp() override {
		// Let's create some listeners for the JobPool
		wakeupDescVec.resize(kNrListeners);
		jobPool = std::make_unique<JobPool>("TestPool", 4, 10, kNrListeners, wakeupDescVec);
		for (uint32_t i = 0; i < kNrListeners; ++i) {
			processingCount[i] = 0;
		}
		for (uint32_t i = 0; i < kNrOperationTypes; ++i) {
			counters[i] = 0;
		}
		startServePoll();
	}

	void TearDown() override {
		stopServePoll();
		jobPool.reset();
	}

	void startServePoll() {
		terminate = false;
		for (uint32_t i = 0; i < kNrListeners; ++i) {
			servePollThreads.emplace_back(&JobPoolTest::servePoll, this, i);
		}
	}

	void stopServePoll() {
		std::unique_lock lock(mutex_);
		terminate = true;
		lock.unlock();

		for (auto &thread : servePollThreads) {
			if (thread.joinable()) {
				thread.join();
			}
		}
		servePollThreads.clear();
	}

	JobPool::ProcessJobCallback mockProcessJob = []() -> uint8_t {
		return 0;  // Return success status
	};

	std::unique_ptr<JobPool> jobPool;
	std::vector<int> wakeupDescVec;
	std::vector<std::thread> servePollThreads;
	bool terminate;
	std::mutex mutex_;
	static constexpr uint32_t kNrListeners = 4;
	static constexpr uint32_t kNrOperationTypes = 10;
	std::atomic<int> processingCount[kNrListeners];
	std::atomic<int> counters[kNrOperationTypes];
};

// Test job addition
TEST_F(JobPoolTest, AddJob) {
	jobPool->addJob(JobPool::ChunkOperation::Read, mockJobCallback, &counters[0], mockProcessJob);
	EXPECT_EQ(jobPool->getJobCount(), 1U);
}

// Test job processing
TEST_F(JobPoolTest, ProcessJob) {
	const int nrJobsToProcess = 10;
	std::vector<int> expectedCounters(kNrOperationTypes, 0);
	std::vector<int> expectedProcessingCount(kNrListeners, 0);

	std::srand(42);  // Seed for random number generation
	for (int i = 0; i < nrJobsToProcess; ++i) {
		auto opType = std::rand() % kNrOperationTypes;
		auto targetListener = std::rand() % kNrListeners;
		jobPool->addJob(JobPool::ChunkOperation::Read, mockJobCallback, &counters[opType],
		                mockProcessJob, targetListener);

		// Update expected counters
		expectedCounters[opType]++;
		expectedProcessingCount[targetListener]++;

		// Wait for the job to be processed
		std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));

		for (uint32_t j = 0; j < kNrOperationTypes; ++j) {
			EXPECT_EQ(counters[j].load(), expectedCounters[j]);
		}
		for (uint32_t j = 0; j < kNrListeners; ++j) {
			EXPECT_EQ(processingCount[j].load(), expectedProcessingCount[j]);
		}
	}
}

// Test job disabling
TEST_F(JobPoolTest, DisableJob) {
	JobPool::JobCallback mockedJobCallback = [](uint8_t status, void *extra) -> void {
		auto *counter = static_cast<std::atomic<int> *>(extra);
		counter->store(status);
	};

	const uint32_t kOpToDisable = 0;           // Index of the operation to disable
	const uint32_t kOpToNotDisable = 1;        // Index of the operation to not disable
	const uint32_t kListenerToDisable = 0;     // Listener ID to disable the job for
	const uint32_t kListenerToNotDisable = 1;  // Listener ID to not disable the job for

	uint32_t jobIdToNotDisable =
	    jobPool->addJob(JobPool::ChunkOperation::Read, mockedJobCallback,
	                    &counters[kOpToNotDisable], mockProcessJob, kListenerToNotDisable);
	uint32_t jobIdToDisable =
	    jobPool->addJob(JobPool::ChunkOperation::Read, mockedJobCallback, &counters[kOpToDisable],
	                    mockProcessJob, kListenerToDisable);

	// Note the jobIds are equal, but we would only disable the job with the specified listener ID
	EXPECT_EQ(jobIdToDisable, jobIdToNotDisable);

	jobPool->disableJob(jobIdToDisable, kListenerToDisable);

	// Wait for the job to be processed
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));

	EXPECT_EQ(counters[kOpToDisable].load(), SAUNAFS_ERROR_NOTDONE);
	EXPECT_EQ(counters[kOpToNotDisable].load(), SAUNAFS_STATUS_OK);
}

// Test changing callbacks
TEST_F(JobPoolTest, ChangeCallback) {
	const uint32_t kOriginalOpType = 0;     // Index of the operation to change the callback for
	const uint32_t kNewOpType = 1;          // Index of the operation to change the callback to
	const uint32_t kNotChangingOpType = 2;  // Index of the operation to not change the callback for
	const uint32_t kChangingListenerId = 0;     // Listener ID to change the callback for
	const uint32_t kNotChangingListenerId = 1;  // Listener ID to not change the callback for

	uint32_t jobIdToNotChange =
	    jobPool->addJob(JobPool::ChunkOperation::Read, mockJobCallback,
	                    &counters[kNotChangingOpType], mockProcessJob, kNotChangingListenerId);
	uint32_t jobIdToChange =
	    jobPool->addJob(JobPool::ChunkOperation::Read, mockJobCallback, &counters[kOriginalOpType],
	                    mockProcessJob, kChangingListenerId);

	// Note the jobIds are equal, but we would only change the callback for the specified listener
	// ID
	EXPECT_EQ(jobIdToChange, jobIdToNotChange);

	jobPool->changeCallback(jobIdToChange, mockJobCallback, &counters[kNewOpType],
	                        kChangingListenerId);

	// Wait for the job to be processed
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));

	EXPECT_EQ(counters[kOriginalOpType].load(), 0);
	EXPECT_EQ(counters[kNewOpType].load(), 1);
	EXPECT_EQ(counters[kNotChangingOpType].load(), 1);
}

// Test job status handling
TEST_F(JobPoolTest, JobStatusHandling) {
	// Stop the servePoll thread to disable automatic job dispatching
	stopServePoll();

	for (uint32_t i = 0; i < kNrOperationTypes; ++i) {
		jobPool->addJob(JobPool::ChunkOperation::Read, mockJobCallback, &counters[i],
		                mockProcessJob);
	}

	// Wait for the job to be processed
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));

	jobPool->processCompletedJobs();
	for (uint32_t i = 0; i < kNrOperationTypes; ++i) {
		EXPECT_EQ(counters[i].load(), 1);  // Each job should have been processed once
	}
}
