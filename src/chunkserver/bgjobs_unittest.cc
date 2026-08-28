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

#include <atomic>

#include "chunkserver/chunk_generation_fence.h"
#include "errors/saunafs_error_codes.h"

#include <sys/poll.h>
#include <thread>

#include "common/slice_traits.h"

#include "gtest/gtest.h"

void mockJobCallback(uint8_t  /*status*/, void *extra) {
	auto *counter = static_cast<std::atomic<int> *>(extra);
	counter->fetch_add(1);
}

void mockJobCallbackDoubling(uint8_t  /*status*/, void *extra) {
	auto *counter = static_cast<std::atomic<int> *>(extra);
	counter->fetch_add(counter->load());
}

constexpr uint32_t kWaitTimeMs = 100;

// Test fixture for JobPool tests
class JobPoolTest : public ::testing::Test {
private:
	void servePoll(uint32_t listenerId) {
		std::vector<pollfd> wakeupDescPollFDs = {
		    pollfd{wakeupDescVec[listenerId], POLLIN, 0},
		    pollfd{masterWakeupDescVec[listenerId], POLLIN, 0},
		    pollfd{clientSwitchModeWakeupDescVec[listenerId], POLLIN, 0},
		    pollfd{clientFifoModeWakeupDescVec[listenerId], POLLIN, 0}};

		while (!terminate) {
			int ret = poll(&wakeupDescPollFDs[0], wakeupDescPollFDs.size(), kWaitTimeMs);
			if (ret > 0) {
				if (wakeupDescPollFDs[0].revents & POLLIN) {
					processingCount[listenerId].fetch_add(1);
					jobPool->processCompletedJobs(listenerId);
				}
				if (wakeupDescPollFDs[1].revents & POLLIN) {
					processingCount[listenerId].fetch_add(1);
					masterJobPool->processCompletedJobs(listenerId);
				}
				if (wakeupDescPollFDs[2].revents & POLLIN) {
					processingCount[listenerId].fetch_add(1);
					clientJobPoolSwitch->processCompletedJobs(listenerId);
				}
				if (wakeupDescPollFDs[3].revents & POLLIN) {
					processingCount[listenerId].fetch_add(1);
					clientJobPoolFifo->processCompletedJobs(listenerId);
				}
			}
		}
	}

protected:
	void SetUp() override {
		// Let's create some listeners for the JobPool
		wakeupDescVec.resize(kNrListeners);
		jobPool = std::make_unique<JobPool>("TestPool", 4, 10, kNrListeners, wakeupDescVec);
		jobPool->startWorkers();

		masterWakeupDescVec.resize(kNrListeners);
		masterJobPool = std::make_unique<MasterJobPool>("TestMasterPool", 4, 10, kNrListeners,
		                                                masterWakeupDescVec);

		// For the ClientJobPool, we want to test both IOPriorityMode::Fifo and
		// IOPriorityMode::Switch, so we will initialize it in the test cases instead of here. For
		// now, we can just initialize it with Switch mode as default.
		clientSwitchModeWakeupDescVec.resize(kNrListeners);
		clientJobPoolSwitch =
		    std::make_unique<ClientJobPool>("TestClientPoolSwitch", 1, 10, kNrListeners,
		                                    clientSwitchModeWakeupDescVec, IOPriorityMode::Switch);
		clientFifoModeWakeupDescVec.resize(kNrListeners);
		clientJobPoolFifo =
		    std::make_unique<ClientJobPool>("TestClientPoolFifo", 1, 10, kNrListeners,
		                                    clientFifoModeWakeupDescVec, IOPriorityMode::Fifo);

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
		usleep(1000);  // Simulate some work by sleeping for 1ms
		return 0;  // Return success status
	};

	std::unique_ptr<JobPool> jobPool;
	std::vector<int> wakeupDescVec;
	std::unique_ptr<MasterJobPool> masterJobPool;
	std::vector<int> masterWakeupDescVec;
	std::unique_ptr<ClientJobPool> clientJobPoolSwitch;
	std::vector<int> clientSwitchModeWakeupDescVec;
	std::unique_ptr<ClientJobPool> clientJobPoolFifo;
	std::vector<int> clientFifoModeWakeupDescVec;

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
	// Create a JobPool instance without workers to disable automatic job dispatching
	auto jobPool = std::make_unique<JobPool>("TestPool", 0, 5, kNrListeners, wakeupDescVec);

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

TEST_F(JobPoolTest, ChunkLocking) {
	const uint64_t chunkId1 = 12345;
	const uint64_t chunkId2 = 123456;
	const uint64_t chunkId3 = 1234567;
	const uint64_t chunkId4 = 12345678;
	const ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	ChunkWithType chunkWithType1{chunkId1, chunkType};
	ChunkWithType chunkWithType2{chunkId2, chunkType};
	ChunkWithType chunkWithType3{chunkId3, chunkType};
	ChunkWithType chunkWithType4{chunkId4, chunkType};

	masterJobPool->startChunkLock(mockJobCallback, &counters[0], chunkId1, chunkType, 0);
	masterJobPool->startChunkLock(mockJobCallback, &counters[0], chunkId2, chunkType, 0);
	masterJobPool->startChunkLock(mockJobCallback, nullptr, chunkId4, chunkType, 0);
	masterJobPool->enforceChunkLock(chunkId1, chunkType);
	masterJobPool->enforceChunkLock(chunkId4, chunkType);

	masterJobPool->addJobIfNotLocked(chunkWithType1, JobPool::ChunkOperation::Read, mockJobCallback,
	                                 &counters[0], mockProcessJob);
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));
	// Job should not be processed because chunk is locked and enforced
	EXPECT_EQ(counters[0].load(), 0);

	masterJobPool->addJobIfNotLocked(chunkWithType2, JobPool::ChunkOperation::Read, mockJobCallback,
	                                 &counters[0], mockProcessJob);
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));
	// Job should be processed because chunk is locked but not enforced
	EXPECT_EQ(counters[0].load(), 1);

	masterJobPool->addJobIfNotLocked(chunkWithType3, JobPool::ChunkOperation::Read, mockJobCallback,
	                                 &counters[0], mockProcessJob);
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));
	// Job should be processed because chunk is not locked
	EXPECT_EQ(counters[0].load(), 2);

	masterJobPool->addJobIfNotLocked(chunkWithType4, JobPool::ChunkOperation::Read, mockJobCallback,
	                                 &counters[0], mockProcessJob);
	masterJobPool->addJobIfNotLocked(chunkWithType4, JobPool::ChunkOperation::Read, mockJobCallback,
	                                 &counters[0], mockProcessJob);
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));
	// Job should not be processed because chunk is locked and enforced
	EXPECT_EQ(counters[0].load(), 2);

	masterJobPool->endChunkLock(chunkId1, chunkType, SAUNAFS_STATUS_OK);
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));
	// Now the job for chunkWithType1 should be processed because the lock is released and the
	// callback for the lock job should be called
	EXPECT_EQ(counters[0].load(), 4);

	masterJobPool->endChunkLock(chunkId2, chunkType, SAUNAFS_STATUS_OK);
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));
	// Now the callback for the lock job of chunkWithType2 should be called
	EXPECT_EQ(counters[0].load(), 5);

	masterJobPool->eraseChunkLock(chunkId4, chunkType);
	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));
	// Now the jobs for chunkWithType4 should be processed because the lock is released, but the
	// callback for the lock job should not be called
	EXPECT_EQ(counters[0].load(), 7);
}

TEST_F(JobPoolTest, IOPriorityMode) {
	const int kInitialSwitchCounterValue = 5;
	const int kInitialFifoCounterValue = 7;
	counters[0] = kInitialSwitchCounterValue;
	counters[1] = kInitialFifoCounterValue;

	// Add a series of Read and Write jobs to the ClientJobPool in Switch mode and check the
	// processing order by the final value.
	for (int i = 0; i < 5; ++i) {
		clientJobPoolSwitch->addJob(JobPool::ChunkOperation::Read, mockJobCallback, &counters[0],
		                            mockProcessJob);
	}
	for (int i = 0; i < 5; ++i) {
		clientJobPoolSwitch->addJob(JobPool::ChunkOperation::Write, mockJobCallbackDoubling,
		                            &counters[0], mockProcessJob);
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));

	// In Switch mode, the Read and Write jobs should be processed in an alternating manner, so
	// the final value should reflect the interleaving of the operations.
	int switchCounterExpectedFinalValue = kInitialSwitchCounterValue;
	for (int i = 0; i < 5; ++i) {
		// Each Read job adds 1, and each Write job doubles the current value.
		switchCounterExpectedFinalValue = (switchCounterExpectedFinalValue + 1) * 2;
	}

	EXPECT_EQ(counters[0].load(), switchCounterExpectedFinalValue);

	// Add a series of Read and Write jobs to the ClientJobPool in Fifo mode and check the
	// processing order by the final value.
	for (int i = 0; i < 5; ++i) {
		clientJobPoolFifo->addJob(JobPool::ChunkOperation::Read, mockJobCallback, &counters[1],
		                          mockProcessJob);
	}
	for (int i = 0; i < 5; ++i) {
		clientJobPoolFifo->addJob(JobPool::ChunkOperation::Write, mockJobCallbackDoubling,
		                          &counters[1], mockProcessJob);
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));

	// In Fifo mode, the Read jobs should be processed first followed by the Write jobs, so the
	// final value should reflect the sequential processing of the operations.
	int fifoCounterExpectedFinalValue = kInitialFifoCounterValue;
	for (int i = 0; i < 5; ++i) {
		// Each Read job adds 1.
		fifoCounterExpectedFinalValue++;
	}
	for (int i = 0; i < 5; ++i) {
		// Each Write job doubles the current value.
		fifoCounterExpectedFinalValue *= 2;
	}

	EXPECT_EQ(counters[1].load(), fifoCounterExpectedFinalValue);
}

void generationStatusCallback(uint8_t status, void *extra) {
	static_cast<std::atomic<int> *>(extra)->store(status);
}

TEST_F(JobPoolTest, VersionJobBelowGenerationHighWaterReturnsBusy) {
	// The execution-time half of the generation fence: a version job whose round generation is
	// below the chunk's recorded high water returns busy from the worker without touching the
	// disk. The pool machinery is real; only the fence decides.
	stopServePoll();
	chunk_generation_fence::resetForTest();
	ASSERT_TRUE(chunk_generation_fence::admit(4242, 7, "accept"));

	std::atomic<int> delivered{-1};
	job_version(*masterJobPool, generationStatusCallback, &delivered, 4242, 1,
	            slice_traits::standard::ChunkPartType(), 2, 6);
	for (int i = 0; i < 400 && delivered.load() == -1; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		masterJobPool->processCompletedJobs();
	}
	EXPECT_EQ(SAUNAFS_ERROR_CHUNKBUSY, delivered.load());
	chunk_generation_fence::resetForTest();
}
