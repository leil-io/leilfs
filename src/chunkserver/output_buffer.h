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

constexpr uint8_t kNotSaunafsStatus = 255;

class OutputBuffer {
public:
	enum class WriteStatus : uint8_t {
		Done,
		Again,
		Error
	};

	enum class BufferType : uint8_t {
		Block,
		CRC,
		Header
	};

	template <typename C = std::vector<uint8_t>>
	class Buffer {
	public:
		Buffer(size_t capacity, size_t padding = 0)
		    : capacity_(capacity),
		      trueCapacity_(capacity + padding),
		      padding_(padding),
		      unflushedDataFirstIndex_(padding),
		      unflushedDataOneAfterLastIndex_(padding),
		      data_(trueCapacity_) {
			eassert(trueCapacity_ > 0);
			data_.reserve(trueCapacity_);
		}
		~Buffer() = default;

		ssize_t copyFromBuffer(const void *mem, size_t len) {
			eassert(unflushedDataFirstIndex_ + len <= unflushedDataOneAfterLastIndex_);
			memcpy((void *)mem, &data_[unflushedDataFirstIndex_], len);
			unflushedDataFirstIndex_ += len;
			return len;
		}
		ssize_t copyIntoBuffer(const void *mem, size_t len) {
			eassert(unflushedDataOneAfterLastIndex_ + len <= trueCapacity_);
			memcpy((void *)&data_[unflushedDataOneAfterLastIndex_], mem, len);
			unflushedDataOneAfterLastIndex_ += len;
			return len;
		}
		ssize_t copyValueIntoBuffer(uint8_t value, size_t len) {
			eassert(unflushedDataOneAfterLastIndex_ + len <= trueCapacity_);
			memset((void *)&data_[unflushedDataOneAfterLastIndex_], value, len);
			unflushedDataOneAfterLastIndex_ += len;
			return len;
		}
		ssize_t copyIntoBuffer(IChunk *chunk, size_t len, off_t offset) {
			eassert(unflushedDataOneAfterLastIndex_ + len <= trueCapacity_);
			off_t bytes_written = 0;
			while (len > 0) {
				ssize_t ret = chunk->owner()->preadData(
				    chunk, &data_[unflushedDataOneAfterLastIndex_], len, offset);
				if (ret <= 0) { return bytes_written; }
				len -= ret;
				unflushedDataOneAfterLastIndex_ += ret;
				bytes_written += ret;
			}
			return bytes_written;
		}
		void clear() {
			unflushedDataFirstIndex_ = padding_;
			unflushedDataOneAfterLastIndex_ = padding_;
		}
		inline size_t capacity() const { return capacity_; }
		inline size_t bytesInABuffer() const {
			return unflushedDataOneAfterLastIndex_ - unflushedDataFirstIndex_;
		}
		inline const uint8_t *paddedIndex(size_t index) const {
			assert(index < capacity_);
			return data_.data() + index + padding_;
		}
		inline const uint8_t *getUnflushedDataFirstIndex() const {
			return &data_[unflushedDataFirstIndex_];
		}
		inline void moveUnflushedDataFirstIndex(int64_t offset) {
			unflushedDataFirstIndex_ += offset;
		}

	private:
		const size_t capacity_;
		const size_t trueCapacity_;
		const size_t padding_;
		size_t unflushedDataFirstIndex_;
		size_t unflushedDataOneAfterLastIndex_;
		C data_;
	};

	explicit OutputBuffer(size_t headerSize, size_t numBlocks);
	~OutputBuffer() = default;

	bool checkCRC(size_t bytes, uint32_t crc, uint32_t startingOffset) const;

	ssize_t copyIntoBuffer(BufferType type, IChunk *chunk, size_t len, off_t offset);
	ssize_t copyIntoBuffer(BufferType type, const void *mem, size_t len);
	ssize_t copyIntoBuffer(BufferType type, const std::vector<uint8_t> &mem);
	ssize_t copyValueIntoBuffer(BufferType type, uint8_t value, size_t len);

	WriteStatus writeOutToAFileDescriptor(int outputFileDescriptor);

	size_t bytesInABuffer() const;
	inline std::pair<size_t, size_t> type() const { return {headerSize_, numBlocks_}; }
	const uint8_t *rawData(BufferType type) const;
	void clear();

	std::atomic_uint8_t status{kNotSaunafsStatus};

private:
	// The current remaining bytes to be written to the file descriptor at once.
	// When the buffer is prepared, should be equal to: SFSBLOCKSIZE + kCrcSize + headerSize_.
	// 0 means that the buffer is not prepared.
	size_t currentRemainingBytesForFD_;
	const size_t headerSize_;
	const size_t numBlocks_;

	Buffer<std::vector<uint8_t, AlignedAllocator<uint8_t, disk::kIoBlockSize>>> blockBuffer_;
	Buffer<std::vector<uint8_t>> crcBuffer_;
	Buffer<std::vector<uint8_t>> headerBuffer_;
};

using OutputBufferPool = BuffersPool<OutputBuffer>;

inline OutputBufferPool &getReadOutputBufferPool() {
	static OutputBufferPool readOutputBuffersPool;
	return readOutputBuffersPool;
}
