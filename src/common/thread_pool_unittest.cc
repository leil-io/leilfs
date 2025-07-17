/*
   Copyright 2025      Leil Storage OÜ

   This file is part of SaunaFS

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

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "thread_pool.h"

using safs::ThreadPool;

// ------------------ basic smoke test ------------------
TEST(ThreadPool, EnqueueReturnsCorrectValue) {
	auto &threadPool = ThreadPool::instance();
	auto future = threadPool.enqueue([](int x, int y) { return x + y; }, 3, 4);
	EXPECT_EQ(future.get(), 7);
}

// ------------------ parallel-for correctness ------------------
TEST(ThreadPool, ParallelForSum) {
	constexpr std::size_t N = 1'000'000;
	std::atomic<long long> sum{0};

	ThreadPool::instance().parallelFor(
	    N, [&](std::size_t i) { sum.fetch_add(i, std::memory_order_relaxed); });

	long long expected = N * (N - 1) / 2;
	EXPECT_EQ(sum.load(), expected);
}

// ------------------ concurrency stress ------------------
TEST(ThreadPool, ManyTasksDoNotRace) {
	constexpr int kTasks = 10'000;
	std::atomic<int> counter{0};

	std::vector<std::future<void>> futures;
	futures.reserve(kTasks);

	for (int i = 0; i < kTasks; ++i) {
		futures.emplace_back(ThreadPool::instance().enqueue([&] { counter.fetch_add(1); }));
	}

	for (auto &future : futures) {
		future.get();
	}
	EXPECT_EQ(counter.load(), kTasks);
}

// ------------------ exception propagation ------------------
TEST(ThreadPool, ExceptionPropagatesToFuture) {
	auto future = ThreadPool::instance().enqueue([] {
		throw std::runtime_error("boom");
	});

	EXPECT_THROW(future.get(), std::runtime_error);
}

// ------------------ empty parallel-for edge case ------------------
TEST(ThreadPool, ParallelForZeroCountDoesNothing) {
	bool touched = false;
	ThreadPool::instance().parallelFor(0, [&](std::size_t) { touched = true; });
	EXPECT_FALSE(touched);
}

// ------------------ map-reduce example ------------------
TEST(ThreadPool, MapReduce) {
	constexpr std::size_t N = 1'000'000;
	std::vector<long long> data(N);
	ThreadPool &threadPool = ThreadPool::instance();

	// 1. map: square each element
	threadPool.parallelFor(N, [&](std::size_t i) { data[i] = static_cast<long long>(i) * i; });

	// 2. reduce: sum the squares
	std::atomic<long long> total{0};
	threadPool.parallelFor(N, [&](std::size_t i) { total.fetch_add(data[i], std::memory_order_relaxed); });

	// closed form: Σ 0²..(N-1)² = (N-1)N(2N-1)/6
	long long expected = (N - 1) * N * (2 * N - 1) / 6;
	EXPECT_EQ(total.load(), expected);
}

// ------------------ producer-consumer example ------------------
template <typename T>
class BoundedQueue {
public:
	explicit BoundedQueue(std::size_t capacity) : cap_(capacity) {}

	void push(const T &value) {
		std::unique_lock lock(mutex_);
		notFull_.wait(lock, [this] { return queue_.size() < cap_; });
		queue_.push(value);
		notEmpty_.notify_one();
	}

	bool pop(T &out) {
		std::unique_lock lock(mutex_);
		notEmpty_.wait(lock, [this] { return !queue_.empty(); });
		out = std::move(queue_.front());
		queue_.pop();
		notFull_.notify_one();
		return true;
	}

private:
	std::size_t cap_;
	std::queue<T> queue_;
	std::mutex mutex_;
	std::condition_variable notEmpty_;
	std::condition_variable notFull_;
};

TEST(ThreadPool, RealProducerConsumer) {
	constexpr int kItems = 1'000;

	BoundedQueue<int> queue(64);
	std::vector<std::uint8_t> seen(kItems, 0);  // 0 = unseen, 1 = seen
	std::mutex seenMutex;

	// Producer thread
	std::thread producer([&] {
		for (int i = 0; i < kItems; ++i) { queue.push(i); }
	});

	// Consumer tasks
	std::latch done(kItems);
	for (int i = 0; i < kItems; ++i) {
		ThreadPool::instance().enqueue([&] {
			int value{0};
			bool ok = queue.pop(value);  // blocks until available
			ASSERT_TRUE(ok);             // should always succeed
			{
				std::lock_guard lock(seenMutex);
				ASSERT_FALSE(seen[value]);  // duplicate detection
				seen[value] = 1;
			}
			done.count_down();
		});
	}

	producer.join();
	done.wait();

	// every value seen exactly once?
	for (int i = 0; i < kItems; ++i) { EXPECT_TRUE(seen[i]) << "missing " << i; }
}

// ------------------ async file hash example ------------------
TEST(ThreadPool, AsyncFileHash) {
	// Fake file → a vector of 4 MB
	constexpr std::size_t kBytes = 4 * 1024 * 1024;
	std::vector<char> file(kBytes, 'A');

	// Each 64 kB chunk → SHA-256 (simplified)
	constexpr std::size_t kChunk = 64 * 1024;
	std::vector<std::future<int>> futures;

	for (std::size_t off = 0; off < kBytes; off += kChunk) {
		futures.emplace_back(ThreadPool::instance().enqueue([&, off] {
			int sum = 0;
			std::size_t end = std::min(off + kChunk, kBytes);
			for (std::size_t i = off; i < end; ++i) {
				sum += file[i];  // toy hash
			}
			return sum;
		}));
	}

	int grandSum = 0;
	for (auto &future : futures) { grandSum += future.get(); }

	EXPECT_EQ(grandSum, static_cast<int>(kBytes * 'A'));  // 65 * 4 M
}

// ------------------ parallel-for exception handling ------------------
TEST(ThreadPool, ParallelForThrows) {
	EXPECT_THROW(
	    ThreadPool::instance().parallelFor(100,
	                                       [](std::size_t i) {
		                                       if (i == 42) {
			                                       throw std::runtime_error("slice 42 failed");
		                                       }
	                                       }),
	    std::runtime_error);  // latch wait rethrows the first exception
}

// ------------------ micro-benchmark for empty tasks (optional) ------------------
TEST(ThreadPool, MicroBenchmarkEmptyTask) {
	constexpr int kTasks = 100'000;
	auto t0 = std::chrono::steady_clock::now();

	std::vector<std::future<void>> futures;
	futures.reserve(kTasks);
	for (int i = 0; i < kTasks; ++i) {
		futures.emplace_back(ThreadPool::instance().enqueue([] {}));
	}

	for (auto &future : futures) {
		future.get();
	}

	auto ms =
	    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	std::cout << "Empty task throughput: " << kTasks / ms * 1000 << " ops/s\n";
}

// ------------------ benchmark fixture (optional) ------------------
TEST(ThreadPool, BenchmarkParallelFor) {
	constexpr std::size_t N = 100'000'000;
	std::atomic<long long> sum{0};

	auto t0 = std::chrono::steady_clock::now();
	ThreadPool::instance().parallelFor(
	    N, [&](std::size_t i) { sum.fetch_add(i, std::memory_order_relaxed); });
	auto t1 = std::chrono::steady_clock::now();

	long long expected = N * (N - 1) / 2;
	EXPECT_EQ(sum.load(), expected);

	std::cout << "Parallel sum took " << std::chrono::duration<double>(t1 - t0).count() << " s\n";
}
