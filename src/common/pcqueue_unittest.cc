/*
   Copyright 2024      Leil Storage OÜ

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

#include "common/pcqueue.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

// Custom deleter for testing
void customDeleter(uint8_t *data) { delete[] data; }

const int kMaxSize = 10;
const uint32_t kMaxLength = 10;
const uint32_t kMaxQueueSize = 100;

// Test adding and removing a single element
TEST(ProducerConsumerQueueTests, SingleElement) {
	ProducerConsumerQueue queue(kMaxSize, customDeleter);
	auto *data = new uint8_t[kMaxLength];
	EXPECT_TRUE(queue.tryPut(1, 1, data, kMaxLength));

	uint32_t jobId = 0;
	uint32_t jobType = 0;
	uint32_t length = 0;
	uint8_t *retrievedData = nullptr;

	queue.get(&jobId, &jobType, &retrievedData, &length);
	EXPECT_EQ(jobId, 1U);
	EXPECT_EQ(jobType, 1U);
	EXPECT_EQ(length, kMaxLength);
	EXPECT_EQ(retrievedData, data);
}

// Test adding and removing multiple elements
TEST(ProducerConsumerQueueTests, MultipleElements) {
	ProducerConsumerQueue queue(kMaxQueueSize, customDeleter);
	for (int i = 0; i < kMaxSize; ++i) {
		auto *data = new uint8_t[kMaxLength];
		EXPECT_TRUE(queue.tryPut(i, i, data, kMaxLength));
	}

	for (uint32_t i = 0; i < kMaxSize; ++i) {
		uint32_t jobId = 0;
		uint32_t jobType = 0;
		uint32_t length = 0;
		uint8_t *retrievedData = nullptr;

		queue.get(&jobId, &jobType, &retrievedData, &length);
		EXPECT_EQ(jobId, i);
		EXPECT_EQ(jobType, i);
		EXPECT_EQ(length, kMaxLength);
	}
}

// Test behavior when the queue is full
TEST(ProducerConsumerQueueTests, QueueFull) {
	ProducerConsumerQueue queue(2, customDeleter);
	auto *data1 = new uint8_t[kMaxLength];
	auto *data2 = new uint8_t[kMaxLength];
	auto *data3 = new uint8_t[kMaxLength];
	EXPECT_TRUE(queue.tryPut(1, 1, data1, 1));
	EXPECT_TRUE(queue.tryPut(2, 2, data2, 1));
	EXPECT_FALSE(queue.tryPut(3, 3, data3, kMaxLength));
	delete[] data3;
}

// Test behavior when the queue is empty
TEST(ProducerConsumerQueueTests, QueueEmpty) {
	ProducerConsumerQueue queue(kMaxSize, customDeleter);
	uint32_t jobId = 0;
	uint32_t jobType = 0;
	uint32_t length = 0;
	uint8_t *retrievedData = nullptr;
	EXPECT_FALSE(queue.tryGet(&jobId, &jobType, &retrievedData, &length));
}

// Test concurrent access by multiple producer threads
TEST(ProducerConsumerQueueTests, MultipleProducers) {
	const int kMaxProducersSize = 10;
	const int kMaxInsertionsPerThread = 10;

	ProducerConsumerQueue queue(kMaxQueueSize, customDeleter);
	std::vector<std::thread> producers;
	producers.reserve(kMaxProducersSize);

	for (int i = 0; i < kMaxProducersSize; ++i) {
		producers.emplace_back([&queue, i]() {
			for (int j = 0; j < kMaxInsertionsPerThread; ++j) {
				auto *data = new uint8_t[kMaxLength];
				queue.put(i * kMaxSize + j, i, data, 1);
			}
		});
	}

	for (auto &producer : producers) { producer.join(); }
	EXPECT_EQ(queue.elements(), kMaxQueueSize);
}

// Test concurrent access by multiple consumer threads
TEST(ProducerConsumerQueueTests, MultipleConsumers) {
	const int kMaxConsumersSize = 10;
	const int kMaxRemovalsPerThread = 10;

	ProducerConsumerQueue queue(kMaxQueueSize, customDeleter);
	for (uint32_t i = 0; i < kMaxQueueSize; ++i) {
		auto *data = new uint8_t[kMaxLength];
		queue.put(i, i, data, 1);
	}

	std::vector<std::thread> consumers;
	consumers.reserve(kMaxConsumersSize);

	for (int i = 0; i < kMaxConsumersSize; ++i) {
		consumers.emplace_back([&queue]() {
			for (int j = 0; j < kMaxRemovalsPerThread; ++j) {
				uint32_t jobId = 0;
				uint32_t jobType = 0;
				uint32_t length = 0;
				uint8_t *retrievedData = nullptr;
				queue.get(&jobId, &jobType, &retrievedData, &length);
			}
		});
	}

	for (auto &consumer : consumers) { consumer.join(); }
	EXPECT_TRUE(queue.isEmpty());
}

// Test concurrent access by both producer and consumer threads
TEST(ProducerConsumerQueueTests, ProducersAndConsumers) {
	const int kMaxProducersSize = 10;
	const int kMaxConsumersSize = 10;
	const int kMaxInsertionsPerThread = 10;
	const int kMaxRemovalsPerThread = 10;

	ProducerConsumerQueue queue(kMaxQueueSize, customDeleter);
	std::vector<std::thread> producers;
	producers.reserve(kMaxProducersSize);

	std::vector<std::thread> consumers;
	consumers.reserve(kMaxConsumersSize);

	for (int i = 0; i < kMaxProducersSize; ++i) {
		producers.emplace_back([&queue, i]() {
			for (int j = 0; j < kMaxInsertionsPerThread; ++j) {
				auto *data = new uint8_t[kMaxLength];
				queue.put(i * kMaxProducersSize + j, i, data, kMaxLength);
			}
		});
	}

	for (int i = 0; i < kMaxConsumersSize; ++i) {
		consumers.emplace_back([&queue]() {
			for (int j = 0; j < kMaxRemovalsPerThread; ++j) {
				uint32_t jobId = 0;
				uint32_t jobType = 0;
				uint32_t length = 0;
				uint8_t *retrievedData = nullptr;
				queue.get(&jobId, &jobType, &retrievedData, &length);
			}
		});
	}

	for (auto &producer : producers) { producer.join(); }
	for (auto &consumer : consumers) { consumer.join(); }

	EXPECT_TRUE(queue.isEmpty());
}

// Test using a custom deleter
TEST(ProducerConsumerQueueTests, CustomDeleter) {
	bool deleterCalled = false;
	auto customDeleter = [&deleterCalled](uint8_t *data) {
		deleterCalled = true;
		delete[] data;
	};

	{
		ProducerConsumerQueue queue(kMaxSize, customDeleter);
		auto *data = new uint8_t[kMaxLength];
		queue.put(1, 1, data, kMaxLength);
	}

	EXPECT_TRUE(deleterCalled);
}

// Test expected ordering with multiple priorities
TEST(ProducerConsumerQueueTests, MultiplePrioritiesBasic) {
	const int kPriorityLevels = 3;
	const int kInsertionsPerPriority = 10;
	const int kNumberOfInsertions = kPriorityLevels * kInsertionsPerPriority;
	const int kBiggerMaxQueueSize = 100000;

	ProducerConsumerQueueWithPriority queue(kPriorityLevels, kBiggerMaxQueueSize, customDeleter);
	for (int i = 0; i < kNumberOfInsertions; ++i) {
		auto *data = new uint8_t[kMaxLength];
		queue.put(i, i, data, kMaxLength, i % kPriorityLevels);
	}

	for (int i = 0; i < kNumberOfInsertions; ++i) {
		uint32_t jobId = 0;
		uint32_t jobType = 0;
		uint32_t length = 0;
		uint8_t *retrievedData = nullptr;

		queue.get(&jobId, &jobType, &retrievedData, &length);
		EXPECT_EQ(jobId, jobType);
		/// order should be: 0, 3, 6, 9, ... 1, 4, 7, 10, ... 2, 5, 8, 11, ...
		/// sorted by priority first (remainder), then by insertion order
		EXPECT_EQ(jobId, static_cast<uint32_t>(i / kInsertionsPerPriority +
		                                       (i % kInsertionsPerPriority) * kPriorityLevels));
		EXPECT_EQ(length, kMaxLength);
	}

	EXPECT_TRUE(queue.isEmpty());
}

// Test expected ordering with multiple priorities with concurrent put/get operations
TEST(ProducerConsumerQueueTests, MultiplePrioritiesConcurrent) {
	const int kPriorityLevels = 3;
	const int kInsertionsPerTick = 10;
	const int kTicks = 50;
	const int kNumberOfThreads = 10;
	const int kBiggerMaxQueueSize = 100000;

	ProducerConsumerQueueWithPriority queue(kPriorityLevels, kBiggerMaxQueueSize, customDeleter);
	std::mutex frequencyMutex;
	std::vector<int> frequency(kPriorityLevels, 0);

	std::vector<std::thread> producers;
	producers.reserve(kNumberOfThreads);
	for (int i = 0; i < kNumberOfThreads; ++i) {
		producers.emplace_back([&queue, &frequency, &frequencyMutex]() {
			// Seed the random number generator with a combination of time and thread ID
			srand(static_cast<unsigned int>(time(nullptr)) ^
			      std::hash<std::thread::id>()(std::this_thread::get_id()));

			for (int j = 0; j < kTicks; ++j) {
				for (int k = 0; k < kInsertionsPerTick; ++k) {
					std::lock_guard<std::mutex> lock(frequencyMutex);
					int basePriority = rand();
					int priority = basePriority % kPriorityLevels;
					uint8_t *data = nullptr;
					if (queue.tryPut(basePriority, priority, data, kMaxLength, priority)) {
						frequency[priority]++;
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});
	}

	std::vector<std::thread> consumers;
	consumers.reserve(kNumberOfThreads);
	for (int i = 0; i < kNumberOfThreads; ++i) {
		consumers.emplace_back([&queue, &frequency, &frequencyMutex]() {
			// Let some puts work before starting the gets
			std::this_thread::sleep_for(std::chrono::milliseconds(10));

			for (int j = 0; j < kTicks; ++j) {
				int k = 0;
				while (k < kInsertionsPerTick) {
					uint32_t jobId = 0;
					uint32_t jobType = 0;
					uint32_t length = 0;
					uint8_t *retrievedData = nullptr;

					std::lock_guard<std::mutex> lock(frequencyMutex);
					if (queue.tryGet(&jobId, &jobType, &retrievedData, &length)) {
						EXPECT_EQ(length, kMaxLength);
						EXPECT_EQ(retrievedData, nullptr);
	
						auto basePriority = jobId;
						auto priority = jobType;
						EXPECT_EQ(priority, basePriority % kPriorityLevels);
	
						// All higher priorities should have been consumed already
						for (uint32_t p = 0; p < priority; ++p) { EXPECT_EQ(frequency[p], 0); }
						EXPECT_GT(frequency[priority], 0);
						frequency[priority]--;
	
						k++; // one more successful get
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});
	}

	for (auto &producer : producers) { producer.join(); }
	for (auto &consumer : consumers) { consumer.join(); }

	EXPECT_TRUE(queue.isEmpty());
}

// Test expected order of retrieval with custom priority levels
TEST(ProducerConsumerQueueTests, GetWithCustomPriority) {
	const int kPriorityLevels = 10;
	const int kTotalInsertions = 1000;
	const int kBiggerMaxQueueSize = 1000000;

	ProducerConsumerQueueWithPriority queue(kPriorityLevels, kBiggerMaxQueueSize, customDeleter);
	std::vector<int> frequency(kPriorityLevels, 0);
	std::vector<uint8_t> priorityLevelsToCheck(kPriorityLevels);
	std::iota(priorityLevelsToCheck.begin(), priorityLevelsToCheck.end(), 0);

	const uint32_t kSeed = 123456789U;
	std::mt19937 rng(kSeed);

	for (int i = 0; i < kTotalInsertions; ++i) {
		uint32_t basePriority = rng();
		uint32_t priority = basePriority % kPriorityLevels;
		uint8_t *data = nullptr;
		if (queue.tryPut(basePriority, priority, data, kMaxLength, priority)) {
			frequency[priority]++;
		}
	}

	for (int i = 0; i < kTotalInsertions; ++i) {
		uint32_t jobId = 0;
		uint32_t jobType = 0;
		uint32_t length = 0;
		uint8_t *retrievedData = nullptr;

		std::shuffle(priorityLevelsToCheck.begin(), priorityLevelsToCheck.end(), rng);
		queue.getUsingCustomPriority(&jobId, &jobType, &retrievedData, &length,
		                             priorityLevelsToCheck);

		EXPECT_EQ(length, kMaxLength);
		EXPECT_EQ(retrievedData, nullptr);

		auto basePriority = jobId;
		auto priority = jobType;
		EXPECT_EQ(priority, basePriority % kPriorityLevels);

		// All higher priorities (given the custom priority order) should have been consumed already
		for (uint32_t j = 0; j < kPriorityLevels; ++j) {
			if (priorityLevelsToCheck[j] == priority) { break; }
			EXPECT_EQ(frequency[priorityLevelsToCheck[j]], 0); 
		}

		EXPECT_GT(frequency[priority], 0);
		frequency[priority]--;
	}

	EXPECT_TRUE(queue.isEmpty());
}
