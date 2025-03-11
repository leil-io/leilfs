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

#include "output_buffer.h"
#include "common/platform.h"

#include <fcntl.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include "common/crc.h"
#include "common/massert.h"

OutputBuffer::OutputBuffer(size_t headerSize, size_t numBlocks)
    : currentRemainingBytesForFD_(0),
      headerSize_(headerSize),
      numBlocks_(numBlocks),
      blockBufferCapacity_(numBlocks * SFSBLOCKSIZE),
      blockBufferCapacityAligned_(numBlocks * SFSBLOCKSIZE + disk::kIoBlockSize),
      blockBufferPadding_(blockBufferCapacityAligned_ - blockBufferCapacity_),
      blockBufferUnflushedDataFirstIndex_(blockBufferPadding_),
      blockBufferUnflushedDataOneAfterLastIndex_(blockBufferPadding_),
      blockBuffer_(blockBufferCapacityAligned_, 0),
      crcBufferCapacity_(kCrcSize * numBlocks),
      crcBufferUnflushedDataFirstIndex_(0),
      crcBufferUnflushedDataOneAfterLastIndex_(0),
      crcBuffer_(crcBufferCapacity_, 0),
      headerBufferCapacity_(headerSize * numBlocks),
      headerBufferUnflushedDataFirstIndex_(0),
      headerBufferUnflushedDataOneAfterLastIndex_(0),
      headerBuffer_(headerBufferCapacity_, 0) {
	eassert(blockBufferCapacity_ > 0);
	eassert(crcBufferCapacity_ > 0);
	eassert(headerBufferCapacity_ > 0);
	blockBuffer_.reserve(blockBufferCapacityAligned_);
	crcBuffer_.reserve(crcBufferCapacity_);
	headerBuffer_.reserve(headerBufferCapacity_);
}

OutputBuffer::WriteStatus OutputBuffer::writeOutToAFileDescriptor(int outputFileDescriptor) {
	// Let's write block by block
	while (bytesInABuffer() > 0) {
		if (currentRemainingBytesForFD_ == 0) {
			// Prepare the blockBuffer to write the current block:
			// - move back the unflushed data in block buffer
			// - copy the header
			// - copy the crc
			// - set the currentRemainingBytesForFD_
			blockBufferUnflushedDataFirstIndex_ -= kCrcSize + headerSize_;
			memcpy((void *)&blockBuffer_[blockBufferUnflushedDataFirstIndex_],
			       (void *)&headerBuffer_[headerBufferUnflushedDataFirstIndex_], headerSize_);
			headerBufferUnflushedDataFirstIndex_ += headerSize_;
			assert(headerBufferUnflushedDataFirstIndex_ <= headerBufferUnflushedDataOneAfterLastIndex_);
			memcpy((void *)&blockBuffer_[blockBufferUnflushedDataFirstIndex_ + headerSize_],
			       (void *)&crcBuffer_[crcBufferUnflushedDataFirstIndex_], kCrcSize);
			crcBufferUnflushedDataFirstIndex_ += kCrcSize;
			assert(crcBufferUnflushedDataFirstIndex_ <= crcBufferUnflushedDataOneAfterLastIndex_);
			currentRemainingBytesForFD_ =
			    std::min(SFSBLOCKSIZE + kCrcSize + headerSize_, bytesInABuffer());
		}
		ssize_t ret =
		    ::write(outputFileDescriptor, &blockBuffer_[blockBufferUnflushedDataFirstIndex_],
				currentRemainingBytesForFD_);

		if (ret <= 0) {
			if (ret == 0 || errno == EAGAIN) { return WRITE_AGAIN; }
			return WRITE_ERROR;
		}
		currentRemainingBytesForFD_ -= ret;
		blockBufferUnflushedDataFirstIndex_ += ret;
	}
	return WRITE_DONE;
}

size_t OutputBuffer::bytesInABuffer() const {
	return (blockBufferUnflushedDataOneAfterLastIndex_ - blockBufferUnflushedDataFirstIndex_) +
	       (crcBufferUnflushedDataOneAfterLastIndex_ - crcBufferUnflushedDataFirstIndex_) +
	       (headerBufferUnflushedDataOneAfterLastIndex_ - headerBufferUnflushedDataFirstIndex_);
}

void OutputBuffer::clear() {
	currentRemainingBytesForFD_ = 0;

	blockBufferUnflushedDataFirstIndex_ = blockBufferPadding_;
	blockBufferUnflushedDataOneAfterLastIndex_ = blockBufferPadding_;
	crcBufferUnflushedDataFirstIndex_ = 0;
	crcBufferUnflushedDataOneAfterLastIndex_ = 0;
	headerBufferUnflushedDataFirstIndex_ = 0;
	headerBufferUnflushedDataOneAfterLastIndex_ = 0;

	status = kNotSaunafsStatus;
}

ssize_t OutputBuffer::copyIntoBlockBuffer(IChunk *chunk, size_t len, off_t offset) {
	eassert(len + blockBufferUnflushedDataOneAfterLastIndex_ <= blockBufferCapacityAligned_);
	off_t bytes_written = 0;

	while (len > 0) {
		ssize_t ret = chunk->owner()->preadData(
		    chunk, &blockBuffer_[blockBufferUnflushedDataOneAfterLastIndex_], len, offset);
		if (ret <= 0) {
			return bytes_written;
		}
		len -= ret;
		blockBufferUnflushedDataOneAfterLastIndex_ += ret;
		bytes_written += ret;
	}

	return bytes_written;
}

bool OutputBuffer::checkCRC(size_t bytes, uint32_t crc, uint32_t startingOffset) const {
	startingOffset += blockBufferPadding_;
	assert(startingOffset > 0 && startingOffset < blockBuffer_.size());
	return mycrc32(0, &blockBuffer_[startingOffset], bytes) == crc;
}

ssize_t OutputBuffer::copyIntoCRCBuffer(const void *mem, size_t len) {
	eassert(crcBufferUnflushedDataOneAfterLastIndex_ + len <= crcBufferCapacity_);
	memcpy((void *)&crcBuffer_[crcBufferUnflushedDataOneAfterLastIndex_], mem, len);
	crcBufferUnflushedDataOneAfterLastIndex_ += len;
	return len;
}

ssize_t OutputBuffer::copyIntoBlockBuffer(const void *mem, size_t len) {
	eassert(blockBufferUnflushedDataOneAfterLastIndex_ + len <= blockBufferCapacityAligned_);
	memcpy((void *)&blockBuffer_[blockBufferUnflushedDataOneAfterLastIndex_], mem, len);
	blockBufferUnflushedDataOneAfterLastIndex_ += len;
	return len;
}

ssize_t OutputBuffer::copyValueIntoBlockBuffer(uint8_t value, size_t len) {
	eassert(blockBufferUnflushedDataOneAfterLastIndex_ + len <= blockBufferCapacityAligned_);
	memset((void *)&blockBuffer_[blockBufferUnflushedDataOneAfterLastIndex_], value, len);
	blockBufferUnflushedDataOneAfterLastIndex_ += len;
	return len;
}

ssize_t OutputBuffer::copyIntoHeaderBuffer(const std::vector<uint8_t> &mem) {
	eassert(headerBufferUnflushedDataOneAfterLastIndex_ + mem.size() <= headerBufferCapacity_);
	memcpy((void *)&headerBuffer_[headerBufferUnflushedDataOneAfterLastIndex_], mem.data(),
	       mem.size());
	headerBufferUnflushedDataOneAfterLastIndex_ += mem.size();
	return mem.size();
}
