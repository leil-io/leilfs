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
	enum WriteStatus {
		WRITE_DONE,
		WRITE_AGAIN,
		WRITE_ERROR
	};

	explicit OutputBuffer(size_t headerSize, size_t numBlocks);
	~OutputBuffer() = default;

	ssize_t copyIntoBlockBuffer(IChunk *chunk, size_t len, off_t offset);

	bool checkCRC(size_t bytes, uint32_t crc, uint32_t startingOffset) const;

	ssize_t copyIntoCRCBuffer(const void *mem, size_t len);
	ssize_t copyIntoBlockBuffer(const void *mem, size_t len);
	ssize_t copyIntoBlockBuffer(const std::vector<uint8_t> &mem);
	ssize_t copyIntoHeaderBuffer(const std::vector<uint8_t> &mem);

	WriteStatus writeOutToAFileDescriptor(int outputFileDescriptor);

	size_t bytesInABuffer() const;
	inline std::pair<size_t, size_t> type() const { return {headerSize_, numBlocks_}; }
	inline const uint8_t *blockData() const { return blockBuffer_.data(); }
	void clear();

	std::atomic_uint8_t status{kNotSaunafsStatus};

private:
	// The current remaining bytes to be written to the file descriptor at once.
	// When the buffer is prepared, should be equal to: SFSBLOCKSIZE + kCrcSize + headerSize_.
	// 0 means that the buffer is not prepared.
	size_t currentRemainingBytesForFD_;
	const size_t headerSize_;
	const size_t numBlocks_;

	const size_t blockBufferCapacity_;
	const size_t blockBufferCapacityAligned_;
	const size_t blockBufferPadding_;
	size_t blockBufferUnflushedDataFirstIndex_;
	size_t blockBufferUnflushedDataOneAfterLastIndex_;
	std::vector<uint8_t, AlignedAllocator<uint8_t, disk::kIoBlockSize>> blockBuffer_;

	const size_t crcBufferCapacity_;
	size_t crcBufferUnflushedDataFirstIndex_;
	size_t crcBufferUnflushedDataOneAfterLastIndex_;
	std::vector<uint8_t> crcBuffer_;
	const size_t headerBufferCapacity_;
	size_t headerBufferUnflushedDataFirstIndex_;
	size_t headerBufferUnflushedDataOneAfterLastIndex_;
	std::vector<uint8_t> headerBuffer_;
};

using OutputBufferPool = BuffersPool<OutputBuffer>;

inline OutputBufferPool &getReadOutputBufferPool() {
	static OutputBufferPool readOutputBuffersPool;
	return readOutputBuffersPool;
}
