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

#include <memory>
#include <queue>

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

		auto it = buffersMap_.find({headerSize, numBlocks});
		if (it == buffersMap_.end() || it->second.empty()) {
			// To make sure the allocation is not done under the lock.
			lock.unlock();
			return std::make_shared<T>(headerSize, numBlocks);
		}

		auto &buffers = it->second;
		auto buffer = buffers.front();

		buffers.pop();
		buffer->clear();
		currentSize_--;

		return buffer;
	}

	/**
	 * Puts a buffer back to the pool.
	 * @param buffer The buffer to put back.
	 */
	void put(std::shared_ptr<T> &&buffer) {
		std::unique_lock lock(mutex_);
		auto &buffers = buffersMap_[buffer->type()];
		if (currentSize_ < kMaxSize) {
			buffer->clear();
			buffers.push(std::move(buffer));
			currentSize_++;
		}
		// To make sure the deallocation is not done under the lock.
		lock.unlock();
	}

private:
	/// Maximum number of buffers in the pool.
	static constexpr size_t kMaxSize = 8192;
	/// Current number of buffers in the pool.
	size_t currentSize_ = 0;
	/// Buffers pool map: a queue of buffers for each type of buffer.
	std::map<std::pair<size_t, size_t>, std::queue<std::shared_ptr<T>>> buffersMap_;
	/// Mutex to protect the pool.
	std::mutex mutex_;
};
