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

#include <sys/types.h>
#include <cstdint>
#include <cstring>
#include <vector>

#include "chunkserver-common/chunk_interface.h"
#include "chunkserver/aligned_allocator.h"
#include "chunkserver/buffers_pool.h"

class OutputBuffer {
public:
	enum WriteStatus {
		WRITE_DONE,
		WRITE_AGAIN,
		WRITE_ERROR
	};

	explicit OutputBuffer(size_t internalBufferCapacity);
	~OutputBuffer() = default;

	ssize_t copyIntoBuffer(IChunk *chunk, size_t len, off_t offset);
	ssize_t copyIntoBuffer(const void *mem, size_t len);

	bool checkCRC(size_t bytes, uint32_t crc) const;

	ssize_t copyIntoBuffer(const std::vector<uint8_t>& mem) {
		return copyIntoBuffer(mem.data(), mem.size());
	}

	WriteStatus writeOutToAFileDescriptor(int outputFileDescriptor);

	size_t bytesInABuffer() const;
	inline size_t capacity() const { return internalBufferCapacity_; }
	inline const uint8_t *data() const { return buffer_.data(); }
	void clear();

	static inline size_t getAlignedSize(size_t capacity) {
		size_t remainder = capacity % disk::kIoBlockSize;

		if (remainder == 0) { return capacity; }

		return capacity + disk::kIoBlockSize - remainder;
	}

	std::atomic_bool isReadCompleted{false};

private:
	const size_t internalBufferCapacity_;
	const size_t internalBufferCapacityAligned_;
	const size_t padding_;
	std::vector<uint8_t, AlignedAllocator<uint8_t, disk::kIoBlockSize>> buffer_;
	size_t bufferUnflushedDataFirstIndex_;
	size_t bufferUnflushedDataOneAfterLastIndex_;
};

using OutputBufferPool = BuffersPool<OutputBuffer>;

inline OutputBufferPool &getReadOutputBufferPool() {
	static OutputBufferPool readOutputBuffersPool;
	return readOutputBuffersPool;
}
