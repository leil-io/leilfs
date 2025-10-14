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

#include "common/platform.h"

#include "chunkserver/io_buffers.h"

#include <fcntl.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include "common/crc.h"

OutputBuffer::OutputBuffer(size_t headerSize, size_t numBlocks)
    : currentRemainingBytesForFD_(0),
      headerSize_(headerSize),
      numBlocks_(numBlocks),
      blockBuffer_(numBlocks * SFSBLOCKSIZE, disk::kIoBlockSize),
      crcBuffer_(numBlocks * kCrcSize),
      headerBuffer_(numBlocks * headerSize) {
	gCurrentTotalOutputBufferBlocks += numBlocks_;
}

OutputBuffer::WriteStatus OutputBuffer::writeOutToAFileDescriptor(int outputFileDescriptor) {
	// Let's write block by block
	while (bytesInABuffer() > 0) {
		if (currentRemainingBytesForFD_ == 0) {
			// Prepare the blockBuffer to write the current block:
			// - move back the unflushed data in block buffer kCrcSize bytes back
			// - copy the crc
			// - move back the unflushed data in block buffer headerSize_ bytes back
			// - copy the header
			// - set the currentRemainingBytesForFD_
			blockBuffer_.moveUnflushedDataFirstIndex(-(static_cast<int32_t>(kCrcSize)));
			crcBuffer_.copyFromBuffer(blockBuffer_.getUnflushedDataFirstIndex(), kCrcSize);
			blockBuffer_.moveUnflushedDataFirstIndex(-(static_cast<int32_t>(headerSize_)));
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

	setStatus(kNotSaunafsStatus);
}

bool OutputBuffer::checkCRC(size_t bytes, uint32_t crc, uint32_t startingOffset) const {
	return mycrc32(0, blockBuffer_.paddedIndex(startingOffset), bytes) == crc;
}

ssize_t OutputBuffer::copyIntoBuffer(BufferType type, IChunk *chunk, size_t len, off_t offset) {
	if (type != BufferType::Block) {
		safs::log_warn("(OutputBuffer::{}) Invalid buffer type, using block buffer", __func__);
		type = BufferType::Block;
	}

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
		safs::log_warn("(OutputBuffer::{}) Invalid buffer type", __func__);
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
		safs::log_warn("(OutputBuffer::{}) Invalid buffer type", __func__);
		return 0;
	}
}

ssize_t OutputBuffer::copyValueIntoBuffer(BufferType type, uint8_t value, size_t len) {
	switch (type) {
	case BufferType::Block:
		return blockBuffer_.copyValueIntoBuffer(value, len);
	case BufferType::CRC:
		return crcBuffer_.copyValueIntoBuffer(value, len);
	case BufferType::Header:
		return headerBuffer_.copyValueIntoBuffer(value, len);
	default:
		safs::log_warn("(OutputBuffer::{}) Invalid buffer type", __func__);
		return 0;
	}
}

const uint8_t *OutputBuffer::rawData(BufferType type) const {
	switch (type) {
	case BufferType::Block:
		return blockBuffer_.paddedIndex(0);
	case BufferType::CRC:
		return crcBuffer_.paddedIndex(0);
	case BufferType::Header:
		return headerBuffer_.paddedIndex(0);
	default:
		safs::log_warn("(OutputBuffer::{}) Invalid buffer type", __func__);
		return 0;
	}
}

InputBuffer::InputBuffer(size_t headerSize, size_t numBlocks)
	: headerSize_(headerSize),
	  numBlocks_(numBlocks),
	  blockBuffer_(numBlocks * SFSBLOCKSIZE, disk::kIoBlockSize),
	  headerBuffer_(numBlocks * headerSize) {
	gCurrentTotalInputBufferBlocks += numBlocks_;
}

ssize_t InputBuffer::readFromSocket(int sock, size_t bytesToRead) {
	ssize_t bytesRead = 0;
	auto bytesReadInHeaderBuffer = headerBuffer_.totalBytesPutInBuffer();
	auto targetBytesInHeaderBuffer = writeInfo_.size() * headerSize_;

	if (bytesReadInHeaderBuffer < targetBytesInHeaderBuffer) {
		// If the header buffer is not filled, we read to the header buffer.
		auto headerBytesRead = headerBuffer_.readFromFD(
		    sock, std::min(targetBytesInHeaderBuffer - bytesReadInHeaderBuffer, bytesToRead));

		if (headerBytesRead <= 0) { return headerBytesRead; }

		bytesRead = headerBytesRead;
	}
	bytesReadInHeaderBuffer = headerBuffer_.totalBytesPutInBuffer();

	if (bytesReadInHeaderBuffer == targetBytesInHeaderBuffer &&
	    static_cast<size_t>(bytesRead) < bytesToRead) {
		// If the header buffer is already filled, we would need to read to the block buffer.
		auto blockBytesRead = blockBuffer_.readFromFD(sock, bytesToRead - bytesRead);

		if (blockBytesRead <= 0) {
			if (bytesRead > 0) {
				return bytesRead;  // Return the number of bytes read so far.
			}
			return blockBytesRead;  // Return the error code from reading the block buffer.
		}

		bytesRead += blockBytesRead;
	}
	return bytesRead;
}

ssize_t InputBuffer::writeToSocket(int sock, size_t bytesToWrite) {
	ssize_t bytesWritten = 0;
	size_t bytesInHeaderBuffer = headerBuffer_.bytesInABuffer();

	if (bytesInHeaderBuffer > 0) {
		// If the header buffer is not flushed, we write from the header buffer.
		size_t headerBytesToWrite = std::min(bytesToWrite, bytesInHeaderBuffer);
		auto headerBytesWritten = headerBuffer_.writeToFD(sock, headerBytesToWrite);

		if (headerBytesWritten <= 0) { return headerBytesWritten; }

		bytesWritten = headerBytesWritten;
	}
	bytesInHeaderBuffer = headerBuffer_.bytesInABuffer();

	if (bytesInHeaderBuffer == 0) {
		// If the header buffer is already flushed, we would need to write from the block buffer.
		auto blockBytesWritten = blockBuffer_.writeToFD(sock, bytesToWrite - bytesWritten);

		if (blockBytesWritten <= 0) {
			if (bytesWritten > 0) {
				return bytesWritten;  // Return the number of bytes written so far.
			}
			return blockBytesWritten;  // Return the error code from writing the block buffer.
		}

		bytesWritten += blockBytesWritten;
	}
	return bytesWritten;
}

ssize_t InputBuffer::copyIntoBuffer(BufferType type, const void *mem, size_t len) {
	switch (type) {
	case BufferType::Block:
		return blockBuffer_.copyIntoBuffer(mem, len);
	case BufferType::Header:
		return headerBuffer_.copyIntoBuffer(mem, len);
	default:
		safs::log_warn("(InputBuffer::{}) Invalid buffer type", __func__);
		return 0;
	}
}

const uint8_t *InputBuffer::rawData(BufferType type) const {
	switch (type) {
	case BufferType::Block:
		return blockBuffer_.paddedIndex(0);
	case BufferType::Header:
		return headerBuffer_.paddedIndex(0);
	default:
		safs::log_warn("(InputBuffer::{}) Invalid buffer type", __func__);
		return 0;
	}
}

const uint8_t *InputBuffer::getStartLastWriteOperationHeader() {
	if (writeInfo_.empty()) {
		safs::log_warn("({}) Called without any write operations.", __func__);
		return nullptr;
	}

	// The last write operation's header starts at the end of the header buffer minus the size of
	// the header.
	return headerBuffer_.paddedIndex((writeInfo_.size() - 1) * headerSize_);
}

void InputBuffer::clear() {
	blockBuffer_.clear();
	crcData_.clear();
	headerBuffer_.clear();
	writeInfo_.clear();
	isBeingUpdated_ = false;
}

void InputBuffer::addNewWriteOperation() {
	// Move the unflushed data pointers in the block buffer to the next position 
	// aligned with SFSBLOCKSIZE.
	blockBuffer_.moveUnflushedDataLastIndex(writeInfo_.size() * SFSBLOCKSIZE -
	                                        blockBuffer_.totalBytesPutInBuffer());
	blockBuffer_.moveUnflushedDataFirstIndex(blockBuffer_.bytesInABuffer());

	writeInfo_.emplace_back(0, 0, 0, 0, 0);
	isBeingUpdated_ = true;
}

void InputBuffer::setupLastWriteOperation(uint16_t blockNum, uint32_t offset, uint32_t size,
                                          uint32_t writeId, uint32_t crc) {
	if (writeInfo_.empty()) {
		safs::log_warn("({}) Called without operations. Adding an empty write operation.",
		               __func__);
		addNewWriteOperation();
	}

	auto &lastWriteInfo = writeInfo_.back();
	lastWriteInfo.blockNum = blockNum;
	lastWriteInfo.offset = offset;
	lastWriteInfo.size = size;
	lastWriteInfo.writeId = writeId;
	crcData_.push_back(crc);
	isBeingUpdated_ = false;
}

std::vector<WriteOperation> InputBuffer::getWriteOperations() const {
	std::vector<WriteOperation> operations;

	auto isMergeable = [](const WriteOperation &wp, const WriteInfo &info) {
		// If the operation and the info refer to full block writes and the info is the next block
		// after the operation, we can merge them.
		return wp.endBlock + 1 == info.blockNum && wp.offset == 0 && wp.size == SFSBLOCKSIZE &&
		       info.offset == 0 && info.size == SFSBLOCKSIZE;
	};

	for (uint32_t i = 0; i < writeInfo_.size(); ++i) {
		const auto &info = writeInfo_[i];

		if (!operations.empty() && isMergeable(operations.back(), info)) {
			operations.back().endBlock++;
			operations.back().crcs.push_back(crcData_[i]);
			continue;
		}

		WriteOperation op;
		op.startBlock = info.blockNum;
		op.endBlock = info.blockNum;
		op.buffer = blockBuffer_.paddedIndex(i * SFSBLOCKSIZE);
		op.offset = info.offset;
		op.size = info.size;
		op.crcs.push_back(crcData_[i]);
		operations.push_back(op);
	}

	return operations;
}

void InputBuffer::applyStatuses(std::vector<uint8_t> &statuses) {
	if (statuses.size() > writeInfo_.size()) {
		safs::log_warn("({}) Called with more statuses than writeInfo_. Truncating the statuses.",
		               __func__);
		statuses.resize(writeInfo_.size());
	}

	for (uint32_t i = 0; i < statuses.size(); ++i) { writeInfo_[i].status = statuses[i]; }
}

std::vector<std::pair<uint8_t, uint32_t>> InputBuffer::getStatuses() const {
	std::vector<std::pair<uint8_t, uint32_t>> statusWriteJobIdPairs;
	statusWriteJobIdPairs.reserve(writeInfo_.size());
	for (const auto &info : writeInfo_) {
		statusWriteJobIdPairs.emplace_back(info.status, info.writeId);
	}

	return statusWriteJobIdPairs;
}

bool InputBuffer::isHeaderSizeValid() const {
	return headerBuffer_.totalBytesPutInBuffer() == writeInfo_.size() * headerSize_;
}

uint32_t InputBuffer::getLastWriteId() const {
	if (writeInfo_.empty()) {
		safs::log_warn("({}) Called without any write operations. Returning 0.", __func__);
		return 0;
	}
	return writeInfo_.back().writeId;
}

bool InputBuffer::isFull() const {
	return writeInfo_.size() == numBlocks_;
}

bool InputBuffer::isBeingUpdated() const {
	return isBeingUpdated_;
}

void releaseOldIoBuffers(uint32_t expirationTime_ms) {
	auto initialTotalOutputBufferBlocks = gCurrentTotalOutputBufferBlocks.load();
	auto initialTotalInputBufferBlocks = gCurrentTotalInputBufferBlocks.load();
	auto initialTotalReplicatorBufferBlocks = gCurrentTotalReplicatorBufferBlocks.load();

	getReadOutputBufferPool().releaseOldBuffers(expirationTime_ms);
	getWriteInputBufferPool().releaseOldBuffers(expirationTime_ms);
	getReplicateBuffersPool().releaseOldBuffers(expirationTime_ms);

	// Calculate the number of released blocks. Please note that the number of released
	// could be negative because some buffers could be added to the pools in the meantime.
	int releasedOutputBuffers =
	    initialTotalOutputBufferBlocks - gCurrentTotalOutputBufferBlocks.load();
	int releasedInputBuffers =
	    initialTotalInputBufferBlocks - gCurrentTotalInputBufferBlocks.load();
	int releasedReplicatorBuffers =
	    initialTotalReplicatorBufferBlocks - gCurrentTotalReplicatorBufferBlocks.load();
	// Log the number of released buffers.
	safs::log_debug("({}) Released buffer blocks per operation: read {}, write {}, replicate {}",
	                __func__, releasedOutputBuffers, releasedInputBuffers,
	                releasedReplicatorBuffers);

	// Log the current amount of blocks buffers per operation.
	safs::log_debug(
	    "({}) Current total buffer blocks per operation: read {}, write {}, replicate {}",
	    __func__, gCurrentTotalOutputBufferBlocks.load(), gCurrentTotalInputBufferBlocks.load(),
	    gCurrentTotalReplicatorBufferBlocks.load());
}
