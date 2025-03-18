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
	 * @param capacity Requested buffer capacity.
	 * @return The existent buffer or a newly created one.
	 */
	std::shared_ptr<T> get(size_t capacity) {
		std::lock_guard lock(mutex_);

		if (buffers_.empty()) {
			return std::make_shared<T>(capacity);
		}

		auto buffer = buffers_.front();

		if (buffer->capacity() == capacity) {
			buffers_.pop();
			buffer->clear();

			return buffer;
		}

		return std::make_shared<T>(capacity);
	}

	/**
	 * Puts a buffer back to the pool.
	 * @param buffer The buffer to put back.
	 */
	void put(std::shared_ptr<T> &&buffer) {
		std::lock_guard lock(mutex_);

		if (buffers_.size() < kMaxSize) {
			buffer->clear();
			buffers_.push(std::move(buffer));
		}
	}

private:
	/// Maximum number of buffers in the pool.
	static constexpr size_t kMaxSize = 8192;
	/// Buffers pool (container).
	std::queue<std::shared_ptr<T>> buffers_;
	/// Mutex to protect the pool.
	std::mutex mutex_;
};
