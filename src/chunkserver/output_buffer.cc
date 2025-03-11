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
      blockBuffer_(numBlocks * SFSBLOCKSIZE, disk::kIoBlockSize),
      crcBuffer_(numBlocks * kCrcSize),
      headerBuffer_(numBlocks * headerSize) {}

OutputBuffer::WriteStatus OutputBuffer::writeOutToAFileDescriptor(int outputFileDescriptor) {
	// Let's write block by block
	while (bytesInABuffer() > 0) {
		if (currentRemainingBytesForFD_ == 0) {
			// Prepare the blockBuffer to write the current block:
			// - move back the unflushed data in block buffer
			// - copy the header
			// - copy the crc
			// - set the currentRemainingBytesForFD_
			blockBuffer_.moveUnflushedDataFirstIndex(-(int32_t)(kCrcSize));
			crcBuffer_.copyFromBuffer(blockBuffer_.getUnflushedDataFirstIndex(), kCrcSize);
			blockBuffer_.moveUnflushedDataFirstIndex(-(int32_t)(headerSize_));
			headerBuffer_.copyFromBuffer(blockBuffer_.getUnflushedDataFirstIndex(), headerSize_);
			currentRemainingBytesForFD_ =
			    std::min(SFSBLOCKSIZE + kCrcSize + headerSize_, bytesInABuffer());
		}
		ssize_t ret = ::write(outputFileDescriptor, blockBuffer_.getUnflushedDataFirstIndex(),
		                      currentRemainingBytesForFD_);

		if (ret <= 0) {
			if (ret == 0 || errno == EAGAIN) { return WriteStatus::Again; }
			return WriteStatus::Error;
		}
		currentRemainingBytesForFD_ -= ret;
		blockBuffer_.moveUnflushedDataFirstIndex(ret);
	}
	return WriteStatus::Done;
}

size_t OutputBuffer::bytesInABuffer() const {
	return blockBuffer_.bytesInABuffer() + crcBuffer_.bytesInABuffer() +
	       headerBuffer_.bytesInABuffer();
}

void OutputBuffer::clear() {
	currentRemainingBytesForFD_ = 0;

	blockBuffer_.clear();
	crcBuffer_.clear();
	headerBuffer_.clear();

	isReadCompleted = false;
}

bool OutputBuffer::checkCRC(size_t bytes, uint32_t crc, uint32_t startingOffset) const {
	return mycrc32(0, blockBuffer_.paddedIndex(startingOffset), bytes) == crc;
}

ssize_t OutputBuffer::copyIntoBuffer(BufferType type, IChunk *chunk, size_t len, off_t offset) {
	massert(type == BufferType::Block, "Invalid buffer type");
	return blockBuffer_.copyIntoBuffer(chunk, len, offset);
}

ssize_t OutputBuffer::copyIntoBuffer(BufferType type, const void *mem, size_t len) {
	switch (type) {
	case BufferType::Block:
		return blockBuffer_.copyIntoBuffer(mem, len);
	case BufferType::CRC:
		return crcBuffer_.copyIntoBuffer(mem, len);
	case BufferType::Header:
		return headerBuffer_.copyIntoBuffer(mem, len);
	default:
		safs::log_err("Invalid buffer type");
		return 0;
	}
}

ssize_t OutputBuffer::copyIntoBuffer(BufferType type, const std::vector<uint8_t> &mem) {
	switch (type) {
	case BufferType::Block:
		return blockBuffer_.copyIntoBuffer(mem.data(), mem.size());
	case BufferType::CRC:
		return crcBuffer_.copyIntoBuffer(mem.data(), mem.size());
	case BufferType::Header:
		return headerBuffer_.copyIntoBuffer(mem.data(), mem.size());
	default:
		safs::log_err("Invalid buffer type");
		return 0;
	}
}

ssize_t OutputBuffer::copyValueIntoBuffer(BufferType type, uint8_t value, size_t len) {
	switch (type) {
	case BufferType::Block:
		blockBuffer_.copyValueIntoBuffer(value, len);
	case BufferType::CRC:
		crcBuffer_.copyValueIntoBuffer(value, len);
	case BufferType::Header:
		headerBuffer_.copyValueIntoBuffer(value, len);
	default:
		safs::log_err("Invalid buffer type");
		return 0;
	}
}

const uint8_t *OutputBuffer::rawData(BufferType type) const {
	switch (type) {
	case BufferType::Block:
		return blockBuffer_.data();
	case BufferType::CRC:
		return crcBuffer_.data();
	case BufferType::Header:
		return headerBuffer_.data();
	default:
		safs::log_err("Invalid buffer type");
		return 0;
	}
}
