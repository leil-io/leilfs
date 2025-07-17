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

#pragma once

#include "common/platform.h"

#include <condition_variable>
#include <functional>
#include <future>
#include <latch>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

namespace safs {

/**
 * @class ThreadPool
 * @brief A thread pool for executing tasks in parallel.
 *
 * The ThreadPool class provides a simple and efficient way to execute tasks concurrently.
 * It manages a pool of worker threads that can be used to run tasks in parallel.
 * The pool size is automatically set to the number of hardware concurrency available
 * but can be customized during construction.
 *
 * The ThreadPool class is designed to be a singleton, providing a global instance
 * that can be accessed from anywhere in the application.
 *
 * @note The ThreadPool class is not copyable or movable to ensure the singleton
 *       instance is unique and properly managed.
 */
class ThreadPool {
public:
	/**
	 * @brief Returns the global instance of the ThreadPool.
	 *
	 * The instance is created on the first call and destroyed when the program exits.
	 *
	 * @return A reference to the global ThreadPool instance.
	 */
	static ThreadPool &instance() {
		static ThreadPool pool;
		return pool;
	}

	/**
	 * @brief Deleted copy constructor to prevent copying.
	 */
	ThreadPool(const ThreadPool &) = delete;

	/**
	 * @brief Deleted copy assignment operator to prevent copying.
	 */
	ThreadPool &operator=(const ThreadPool &) = delete;

	/**
	 * @brief Deleted move constructor to prevent moving.
	 */
	ThreadPool(ThreadPool &&) = delete;

	/**
	 * @brief Deleted move assignment operator to prevent moving.
	 */
	ThreadPool &operator=(ThreadPool &&) = delete;

	/**
	 * @brief Enqueues a task to be executed by the thread pool.
	 *
	 * The task is wrapped in a std::future, allowing the caller to wait for its completion.
	 *
	 * @tparam Callable The type of the callable object (function, lambda, etc.).
	 * @tparam Args The types of the arguments to be passed to the callable.
	 * @param callable The callable object to be executed.
	 * @param args The arguments to be passed to the callable.
	 * @return A std::future representing the result of the task.
	 */
	template <typename Callable, typename... Args>
	auto enqueue(Callable &&callable, Args &&...args)
	    -> std::future<std::invoke_result_t<Callable, Args...>>;

	/**
	 * @brief Executes a callable for each item in a range in parallel.
	 *
	 * The range is divided into chunks, and each chunk is processed by a separate worker thread.
	 *
	 * @tparam Callable The type of the callable object (function, lambda, etc.).
	 * @param itemCount The number of items in the range.
	 * @param callable The callable object to be executed for each item.
	 */
	template <typename Callable>
	void parallelFor(std::size_t itemCount, Callable &&callable);

	/**
	 * @brief Returns the number of worker threads in the pool.
	 *
	 * @return The number of worker threads.
	 */
	std::size_t workerCount() const noexcept { return workerThreads.size(); }

private:
	/**
	 * @brief Constructs a new ThreadPool instance with the specified number of worker threads.
	 *
	 * @param workerCount The number of worker threads to create.
	 */
	explicit ThreadPool(std::size_t workerCount = std::thread::hardware_concurrency());

	/**
	 * @brief Destroys the ThreadPool instance.
	 */
	~ThreadPool();

	/**
	 * @brief The main loop of a worker thread.
	 *
	 * The worker thread waits for tasks to be enqueued and executes them.
	 *
	 * @param stopToken A stop token to signal the worker thread to stop.
	 */
	void workerLoop(const std::stop_token &stopToken);

	/**
	 * @brief The vector of worker threads.
	 */
	std::vector<std::jthread> workerThreads;

	/**
	 * @brief The queue of pending jobs.
	 */
	std::queue<std::move_only_function<void()>> pendingJobs;

	/**
	 * @brief The mutex protecting access to the pending jobs queue.
	 */
	std::mutex jobMutex;

	/**
	 * @brief The condition variable used to signal the worker threads.
	 */
	std::condition_variable_any jobReady;
};

// ---------- definitions ----------
inline ThreadPool::ThreadPool(std::size_t workerCount) {
	workerThreads.reserve(workerCount);
	for (std::size_t i = 0; i < workerCount; ++i) {
		workerThreads.emplace_back([this](std::stop_token stopToken) { workerLoop(stopToken); });
	}
}

inline ThreadPool::~ThreadPool() {
	for (auto &thread : workerThreads) {
		thread.request_stop();
	}
	jobReady.notify_all();
}

inline void ThreadPool::workerLoop(const std::stop_token &stopToken) {
	while (!stopToken.stop_requested()) {
		std::move_only_function<void()> job;
		{
			std::unique_lock lock(jobMutex);
			jobReady.wait(lock, stopToken, [this, &stopToken]() {
				return !pendingJobs.empty() || stopToken.stop_requested();
			});
			if (stopToken.stop_requested()) {
				break;
			}
			job = std::move(pendingJobs.front());
			pendingJobs.pop();
		}
		job();
	}
}

template <typename Callable, typename... Args>
inline auto ThreadPool::enqueue(Callable &&callable, Args &&...args)
    -> std::future<std::invoke_result_t<Callable, Args...>> {
	using Result = std::invoke_result_t<Callable, Args...>;
	std::packaged_task<Result()> task(
	    std::bind_front(std::forward<Callable>(callable), std::forward<Args>(args)...));
	std::future<Result> future = task.get_future();
	{
		std::lock_guard lock(jobMutex);
		pendingJobs.push(std::move(task));
	}
	jobReady.notify_one();
	return future;
}

template <typename Callable>
inline void ThreadPool::parallelFor(std::size_t itemCount, Callable &&callable) {
	if (itemCount == 0) {
		return;
	}

	const std::size_t workerCount = workerThreads.size();
	std::latch done(workerCount);
	std::mutex exceptionMutex;
	std::exception_ptr exception;  // single slot for exception

	auto wrapper = [itemCount, &callable, workerCount, &done, &exceptionMutex,
	                &exception](std::size_t workerIndex) {
		try {
			std::size_t begin = (itemCount * workerIndex) / workerCount;
			std::size_t end = (itemCount * (workerIndex + 1)) / workerCount;
			for (std::size_t i = begin; i < end; ++i) {
				callable(i);
			}
		} catch (...) {
			std::lock_guard lock(exceptionMutex);
			if (!exception) {
				exception = std::current_exception();
			}
		}
		done.count_down();
	};

	for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
		enqueue([wrapper, workerIndex] { wrapper(workerIndex); });
	}

	done.wait();
	if (exception) {
		std::rethrow_exception(exception);
	}
}

}  // namespace safs
