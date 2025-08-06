/*
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

#include <map>
#include <memory>
#include <mutex>
#include <queue>

#include "common/time_utils.h"
#include "slogger/slogger.h"

/**
 * BuffersPool is a thread-safe pool of buffers.
 * It is used to avoid memory allocation/deallocation overhead.
 */
template<typename T>
class BuffersPool {
public:
	/// Default constructor.
	BuffersPool() = default;

	// Disable not needed copy/move constructor and assignment operator.
	BuffersPool(const BuffersPool &) = delete;
	BuffersPool &operator=(const BuffersPool &) = delete;
	BuffersPool(BuffersPool &&) = delete;
	BuffersPool &operator=(BuffersPool &&) = delete;

	/// Default destructor.
	~BuffersPool() = default;

	/**
	 * Gets a buffer from the pool or creates a new one.
	 * @param headerSize The size of the header.
	 * @param numBlocks The number of blocks.
	 * @return The existent buffer or a newly created one.
	 */
	std::shared_ptr<T> get(size_t headerSize, size_t numBlocks) {
		std::unique_lock lock(mutex_);

		if (operationsSinceLastLog_ == kLogAfterEveryXTimes) {
			safs::log_trace(
			    "BuffersPool::get {} blocks, currentNumberOfBlocks_ = {}, kMaxNumberOfBlocks = {}",
			    numBlocks, currentNumberOfBlocks_, kMaxNumberOfBlocks);
			operationsSinceLastLog_ = 0;
		}
		operationsSinceLastLog_++;

		auto it = buffersMap_.find({headerSize, numBlocks});
		if (it == buffersMap_.end() || it->second.empty()) {
			// To make sure the allocation is not done under the lock.
			lock.unlock();
			return std::make_shared<T>(headerSize, numBlocks);
		}

		auto &buffers = it->second;
		auto buffer = buffers.front().getBuffer();

		buffers.pop();
		buffer->clear();
		currentNumberOfBlocks_ -= numBlocks;

		return buffer;
	}

	/**
	 * Puts a buffer back to the pool.
	 * @param buffer The buffer to put back.
	 */
	void put(std::shared_ptr<T> &&buffer) {
		std::unique_lock lock(mutex_);

		auto [headerSize, numBlocks] = buffer->type();
		auto &buffers = buffersMap_[{headerSize, numBlocks}];

		if (operationsSinceLastLog_ == kLogAfterEveryXTimes) {
			safs::log_trace(
			    "BuffersPool::put {} blocks, currentNumberOfBlocks_ = {}, kMaxNumberOfBlocks = {}",
			    numBlocks, currentNumberOfBlocks_, kMaxNumberOfBlocks);
			operationsSinceLastLog_ = 0;
		}
		operationsSinceLastLog_++;

		if (currentNumberOfBlocks_ + numBlocks <= kMaxNumberOfBlocks) {
			buffer->clear();
			buffers.emplace(std::move(buffer));
			currentNumberOfBlocks_ += numBlocks;
		}
		// To make sure the deallocation is not done under the lock.
		lock.unlock();
	}

	/**
	 * Release entries older than expirationTime_ms milliseconds.
	 * @param expirationTime_ms Time threshold to release entries from (in ms).
	 */
	void releaseOldBuffers(uint32_t expirationTime_ms) {
		std::unique_lock lock(mutex_);
		std::vector<std::shared_ptr<T>> buffersToRelease;
		for (auto &[_, buffers] : buffersMap_) {
			// Queue behavior ensures older entries are at the front
			while (!buffers.empty() && buffers.front().expired(expirationTime_ms)) {
				auto buffer = buffers.front().getBuffer();
				buffers.pop();
				auto [_, numBlocks] = buffer->type();
				currentNumberOfBlocks_ -= numBlocks;
				buffersToRelease.emplace_back(std::move(buffer));
			}
		}
		// To make sure the releasing is performed outside the lock
		lock.unlock();

		// Destructor of the vector should release the last copy of the buffers
	}

private:
	struct BufferPoolEntry {
		std::shared_ptr<T> entry_;
		Timer timer_;

		BufferPoolEntry(std::shared_ptr<T> &&entry) : entry_(std::move(entry)) {}

		bool expired(uint32_t expirationTime_ms) const {
			return timer_.elapsed_ms() >= expirationTime_ms;
		}

		std::shared_ptr<T> &&getBuffer() { return std::move(entry_); }
	};

	/// How many operations should pass between two log messages.
	static constexpr size_t kLogAfterEveryXTimes = 1000;
	/// Number of operations since last log message.
	size_t operationsSinceLastLog_ = 0;
	/// Maximum number of buffers in the pool.
	static constexpr size_t kMaxNumberOfBlocks = 8192;
	/// Current number of buffers in the pool.
	size_t currentNumberOfBlocks_ = 0;
	/// Buffers pool map: a queue of buffers for each type of buffer.
	std::map<std::pair<size_t, size_t>, std::queue<BufferPoolEntry>> buffersMap_;
	/// Mutex to protect the pool.
	std::mutex mutex_;
};
