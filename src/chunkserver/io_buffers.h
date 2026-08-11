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

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sys/types.h>
#include <vector>

#include "chunkserver-common/chunk_interface.h"
#include "chunkserver/buffers_pool.h"
#include "common/aligned_allocator.h"

inline std::atomic<uint32_t> gCurrentTotalOutputBufferBlocks = 0;
inline std::atomic<uint32_t> gCurrentTotalInputBufferBlocks = 0;
inline std::atomic<uint32_t> gCurrentTotalChunkCopyBufferBlocks = 0;

/// @class Buffer
/// @brief Manages a data buffer.
template <typename ContainerType = std::vector<uint8_t>>
class Buffer {
public:
	/// @brief Constructs a buffer with the given capacity and padding.
	/// @param capacity The capacity of the buffer.
	/// @param padding The padding of the buffer.
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

	/// @brief Default destructor.
	~Buffer() = default;

	/// @brief Copies data from the buffer to the given memory.
	/// This operation counts as flushing the data.
	/// @param mem The memory to copy the data to.
	/// @param len The length of the data to copy.
	/// @return The number of bytes copied.
	ssize_t copyFromBuffer(const void *mem, size_t len) {
		eassert(unflushedDataFirstIndex_ + len <= unflushedDataOneAfterLastIndex_);
		memcpy((void *)mem, &data_[unflushedDataFirstIndex_], len);
		unflushedDataFirstIndex_ += len;
		return len;
	}

	/// @brief Appends data to the buffer from the given memory.
	/// @param mem The memory to copy the data from.
	/// @param len The length of the data to copy.
	/// @return The number of bytes copied.
	ssize_t copyIntoBuffer(const void *mem, size_t len) {
		eassert(unflushedDataOneAfterLastIndex_ + len <= trueCapacity_);
		memcpy((void *)&data_[unflushedDataOneAfterLastIndex_], mem, len);
		unflushedDataOneAfterLastIndex_ += len;
		return len;
	}

	/// @brief Appends a value to the buffer a given number of times.
	/// @param value The value to copy.
	/// @param len The number of times to copy the value.
	/// @return The number of bytes copied.
	ssize_t copyValueIntoBuffer(uint8_t value, size_t len) {
		eassert(unflushedDataOneAfterLastIndex_ + len <= trueCapacity_);
		memset((void *)&data_[unflushedDataOneAfterLastIndex_], value, len);
		unflushedDataOneAfterLastIndex_ += len;
		return len;
	}

	/// @brief Appends data from the given chunk to the buffer.
	/// @param chunk The chunk to copy the data from.
	/// @param len The length of the data to copy.
	/// @param offset The offset to copy the data from.
	/// @return The number of bytes copied.
	ssize_t copyIntoBuffer(IChunk *chunk, size_t len, off_t offset) {
		eassert(unflushedDataOneAfterLastIndex_ + len <= trueCapacity_);
		off_t bytes_written = 0;
		while (len > 0) {
			ssize_t ret = chunk->owner()->preadData(chunk, &data_[unflushedDataOneAfterLastIndex_],
			                                        len, offset);
			if (ret <= 0) { return bytes_written; }
			len -= ret;
			unflushedDataOneAfterLastIndex_ += ret;
			bytes_written += ret;
		}
		return bytes_written;
	}

	/// @brief Overwrites data in the buffer at the given offset.
	/// @param offset The offset to overwrite the data at.
	/// @param mem The memory to copy the data from.
	/// @param len The length of the data to copy.
	void overwriteInterval(size_t offset, const void *mem, size_t len) {
		eassert(offset + len <= capacity_);
		memcpy((void *)&data_[offset + padding_], mem, len);
	}

	ssize_t readFromFD(int sock, size_t len) {
		eassert(unflushedDataOneAfterLastIndex_ + len <= trueCapacity_);
		ssize_t bytesRead = ::read(sock, &data_[unflushedDataOneAfterLastIndex_], len);
		if (bytesRead > 0) {
			unflushedDataOneAfterLastIndex_ += bytesRead;
		}
		return bytesRead;
	}

	ssize_t writeToFD(int sock, size_t len) {
		eassert(unflushedDataFirstIndex_ + len <= unflushedDataOneAfterLastIndex_);
		ssize_t bytesWritten = ::write(sock, &data_[unflushedDataFirstIndex_], len);
		if (bytesWritten > 0) {
			unflushedDataFirstIndex_ += bytesWritten;
		}
		return bytesWritten;
	}

	/// @brief Clears the buffer.
	void clear() {
		unflushedDataFirstIndex_ = padding_;
		unflushedDataOneAfterLastIndex_ = padding_;
	}

	/// @brief Returns the capacity of the buffer.
	inline size_t capacity() const { return capacity_; }

	/// @brief Returns the number of unflushed bytes in the buffer.
	inline size_t bytesInABuffer() const {
		return unflushedDataOneAfterLastIndex_ - unflushedDataFirstIndex_;
	}

	inline size_t totalBytesPutInBuffer() const {
		return unflushedDataOneAfterLastIndex_ - padding_;
	}

	/// @brief Returns the pointer to the given index considering the padding.
	/// @param index The index to get the pointer to.
	inline const uint8_t *paddedIndex(size_t index) const {
		assert(index < capacity_);
		return data_.data() + index + padding_;
	}

	/// @brief Returns the pointer to the first unflushed data.
	inline const uint8_t *getUnflushedDataFirstIndex() const {
		return &data_[unflushedDataFirstIndex_];
	}

	/// @brief Moves the first unflushed data index by the given offset.
	/// @param offset The offset to move the index by.
	inline void moveUnflushedDataFirstIndex(int64_t offset) { unflushedDataFirstIndex_ += offset; }

	inline void moveUnflushedDataLastIndex(int64_t offset) {
		unflushedDataOneAfterLastIndex_ += offset;
	}

private:
	const size_t capacity_;           ///< The capacity of the buffer.
	const size_t trueCapacity_;       ///< The real size of the underlying container.
	const size_t padding_;            ///< The amount of unused space at the start of the container.
	size_t unflushedDataFirstIndex_;  ///< The index of the first unflushed data.
	size_t unflushedDataOneAfterLastIndex_;  ///< The index of the first byte after the last
	                                         ///< unflushed data.
	ContainerType data_;                     ///< The underlying data container.
};

/**
 * @class OutputBuffer
 * @brief Manages the output buffer for writing data to a file descriptor.
 *
 * The OutputBuffer class is responsible for managing the data to be written to
 * a file descriptor. It provides functions to copy data into the buffer and write
 * it to the file descriptor.
 *
 * The buffer is divided into three parts:
 *
 * - Header: The header of the packet.
 *
 * - CRC: The CRC of the block of data.
 *
 * - Block: The data block to be sent.
 *
 * The buffer is prepared to write the data to the file descriptor at once. The order of
 * the final write interleaves the header, the CRC, and the block. All the headers are
 * supposed to have the same length, which must be provided during the buffer creation.
 * The sizes of CRC (kCrcSize) and the block (SFSBLOCKSIZE) buffers per block are fixed.
 *
 * The block buffer is aligned to the disk I/O block size. To get the expected behavior the only
 * block which may not be complete is the last one.
 */
class OutputBuffer {
public:
	/// @enum WriteStatus
	/// @brief Represents the status of the write operation.
	enum class WriteStatus : uint8_t {
		Done,   ///< The write operation was successful.
		Again,  ///< The write operation should be retried.
		Error   ///< An error occurred during the write operation.
	};

	/// @enum BufferType
	/// @brief Represents the type of buffer.
	enum class BufferType : uint8_t {
		Block,  ///< The block buffer.
		CRC,    ///< The CRC buffer.
		Header  ///< The header buffer.
	};

	/// @brief Constructs an OutputBuffer with the given header size and number of blocks.
	/// @param headerSize The size of the header.
	/// @param numBlocks The number of blocks.
	explicit OutputBuffer(size_t headerSize, size_t numBlocks);

	/// @brief Destructor only decreases the global counter of output buffers blocks.
	~OutputBuffer() {
		gCurrentTotalOutputBufferBlocks -= numBlocks_;
	}

	/// @brief Checks the CRC of the data inside the block buffer.
	/// @param bytes The number of bytes to check.
	/// @param crc The CRC to check.
	/// @param startingOffset The starting offset of the data in the block buffer (without padding).
	/// @return True if the CRC is correct, false otherwise.
	bool checkCRC(size_t bytes, uint32_t crc, uint32_t startingOffset) const;

	/// @brief Copies data from the given chunk into the buffer.
	/// Note: The type must be BufferType::Block.
	/// @param type The type of buffer to copy the data into.
	/// @param chunk The chunk to copy the data from.
	/// @param len The length of the data to copy.
	/// @param offset The offset to copy the data from.
	/// @return The number of bytes copied.
	ssize_t copyIntoBuffer(BufferType type, IChunk *chunk, size_t len, off_t offset);

	/// @brief Copies data from the given memory into the buffer.
	/// @param type The type of buffer to copy the data into.
	/// @param mem The memory to copy the data from.
	/// @param len The length of the data to copy.
	/// @return The number of bytes copied.
	ssize_t copyIntoBuffer(BufferType type, const void *mem, size_t len);

	/// @brief Copies entire data from the given vector into the buffer.
	/// @param type The type of buffer to copy the data into.
	/// @param mem The vector to copy the data from.
	/// @return The number of bytes copied.
	ssize_t copyIntoBuffer(BufferType type, const std::vector<uint8_t> &mem);

	/// @brief Copies a value into the buffer a given number of times.
	/// @param type The type of buffer to copy the value into.
	/// @param value The value to copy.
	/// @param len The number of times to copy the value.
	/// @return The number of bytes copied.
	ssize_t copyValueIntoBuffer(BufferType type, uint8_t value, size_t len);

	/// @brief Writes the data to the given file descriptor.
	/// @param outputFileDescriptor The file descriptor to write the data to.
	/// @return The status of the write operation.
	WriteStatus writeOutToAFileDescriptor(int outputFileDescriptor);

	/// @brief Returns the number of unflushed bytes in the buffer.
	size_t bytesInABuffer() const;

	/// @brief Returns the type of the buffer.
	inline std::pair<size_t, size_t> type() const { return {headerSize_, numBlocks_}; }

	/// @brief Returns the pointer to the beginning (after padding) of the data of the given type.
	const uint8_t *rawData(BufferType type) const;

	/// @brief Clears the buffer.
	void clear();

	/// @brief Returns the `isCallbackStarted` flag.
	/// Whether the buffer's related callback has started processing or has been processed.
	bool isCallbackStarted() const { return isCallbackStarted_; }

	/// @brief Sets the `isProcessed` flag.
	void setIsCallbackStarted(bool newIsCallbackStarted) {
		isCallbackStarted_ = newIsCallbackStarted;
	}

	/// @brief Updates the block data in the buffer for the given block index and the offset and
	/// size in the block.
	/// @param blockIndex The index of the block to update.
	/// @param offsetInBlock The offset within the block to start updating.
	/// @param sizeInBlock The size of the data to update within the block.
	/// @param mem The memory containing the data to update.
	void updateIntervalBlockData(size_t blockIndex, size_t offsetInBlock, size_t sizeInBlock,
	                             const void *mem);

	/// @brief Updates the CRC of the block in the buffer for the given block index and size.
	/// @param blockIndex The index of the block to update the CRC for.
	/// @param size The size of the data block to update its CRC.
	void updateBlockCRC(size_t blockIndex, size_t size);

private:
	/// The current remaining bytes to be written to the file descriptor at once.
	/// When the buffer is prepared, should be equal to: SFSBLOCKSIZE + kCrcSize + headerSize_.
	/// 0 means that the buffer is not prepared or is finished.
	size_t currentRemainingBytesForFD_;
	/// The size of the header.
	const size_t headerSize_;
	/// The number of blocks.
	const size_t numBlocks_;

	/// Whether the buffer's related callback has started processing or has been processed.
	bool isCallbackStarted_{false};

	/// The buffer for the block data.
	Buffer<std::vector<uint8_t, AlignedAllocator<uint8_t, disk::kIoBlockSize>>> blockBuffer_;
	/// The buffer for the CRC data.
	Buffer<std::vector<uint8_t>> crcBuffer_;
	/// The buffer for the header data.
	Buffer<std::vector<uint8_t>> headerBuffer_;
};

/// @struct WriteInfo
/// @brief Contains information about a write operation directly coming from a WRITE_DATA packet.
/// This structure is used to store the information about the blocks to be written.
struct WriteInfo {
	uint16_t blockNum;  ///< The block number in the chunk.
	uint32_t offset;    ///< The offset in the block where the data starts.
	uint32_t size;      ///< The size of the data to be written.
	uint32_t writeId;   ///< The write identifier from client.
	uint8_t status;     ///< The status of the write operation.
};

/// @struct WriteOperation
/// @brief Represents a write operation to be performed by the JobPool workers.
///
/// The operation consists of a possibly condensed set of blocks to be written
/// to the disk. The blocks are written in the same order as they were received
/// from the client. A set of full blocks, not necessarily contiguous in memory, can be written in
/// one write operation and the partial blocks are written one-by-one.
struct WriteOperation {
	uint16_t startBlock;         ///< The start block number in the chunk.
	/// The number of blocks to be written from each buffer.
	std::vector<uint16_t> blocksPerBuffer;
	/// The pointers to the buffers containing the block data.
	std::vector<const uint8_t *> buffers;
	uint32_t offset;             ///< The offset in the block where the data starts.
	uint32_t size;               ///< The size of the data to be written.
	std::vector<uint32_t> crcs;  ///< The CRCs of the blocks to be written.
};

/**
 * @class InputBuffer
 * @brief Manages the input buffer for reading data from a file descriptor and
 * prepares it for writing to the disk.
 *
 * The InputBuffer class is responsible for managing the data read from a file descriptor,
 * most of all the WRITE_DATA packets prefix and the data blocks. It provides functions to
 * copy data into the buffer, manage write operations, and prepare the data for writing to
 * the disk. In the case of chain writes, it also manages the forwarding of data to other
 * chunkservers.
 *
 * The buffer is divided into three parts:
 *
 * - Header: The header of the packet.
 *
 * - Block: The data block to be written.
 *
 * - WriteInfo: The information about the blocks to be written.
 *
 * The Block buffer is aligned to the disk I/O block size. All the headers are
 * supposed to have the same length, which must be provided during the buffer creation.
 *
 * The InputBuffer is prepared to be updated with new data read from the file descriptor while
 * being processed by the JobPool workers.
 */
class InputBuffer {
public:
	/// @enum BufferType
	/// @brief Represents the type of buffer.
	enum class BufferType : uint8_t {
		Block,  ///< The block buffer.
		Header  ///< The header buffer.
	};

	/// @brief Constructs an InputBuffer with the given header size and number of blocks.
	/// @param headerSize The size of the header.
	/// @param numBlocks The number of blocks.
	explicit InputBuffer(size_t headerSize, size_t numBlocks);

	/// @brief Destructor only decreases the global counter of input buffers blocks and write
	/// buffering blocks.
	~InputBuffer();

	/// @brief Reads at most `bytesToRead` bytes from the socket.
	/// It puts the data into the header buffer if not already filled considering the
	/// current number of write operations, and into the block buffer in the other case.
	/// @param sock The socket to read from.
	/// @param bytesToRead The number of bytes to read.
	ssize_t readFromSocket(int sock, size_t bytesToRead);

	/// @brief Writes at most `bytesToWrite` bytes to the socket.
	/// It writes the data from the header buffer if not already flushed, and from the block
	/// buffer in the other case. Used when forwarding data to another chunkserver.
	/// @param sock The socket to write to.
	/// @param bytesToWrite The number of bytes to write.
	ssize_t writeToSocket(int sock, size_t bytesToWrite);

	/// @brief Copies data from the given memory into the buffer.
	/// @param type The type of buffer to copy the data into.
	/// @param mem The memory to copy the data from.
	/// @param len The length of the data to copy.
	/// @return The number of bytes copied.
	ssize_t copyIntoBuffer(BufferType type, const void *mem, size_t len);

	/// @brief Returns the type of the buffer.
	std::pair<size_t, size_t> type() const { return {headerSize_, numBlocks_}; }

	/// @brief Returns the pointer to the beginning (after padding) of the data of the given type.
	const uint8_t *rawData(BufferType type) const;

	/// @brief Returns the pointer to the start of the last write operation header.
	/// If there are no write operations, it returns nullptr and logs a warning.
	/// Used when deserializing the last write operation header or when forwarding data.
	/// @return The pointer to the start of the last write operation header.
	const uint8_t *getStartLastWriteOperationHeader();

	/// @brief Clears the buffer.
	void clear();

	/// @brief Adds a new empty write operation to the buffer.
	/// It also sets block buffer last unflushed index to next one aligned to SFSBLOCKSIZE.
	void addNewWriteOperation();

	/// @brief Sets the last write operation parameters.
	/// It also sets the state to WriteState::BeingUpdatedInqueue.
	/// @param blockNum The block number in the chunk.
	/// @param offset The offset in the block where the data starts.
	/// @param size The size of the data to be written.
	/// @param writeId The write identifier from client.
	/// @param crc The CRC of the block.
	void setupLastWriteOperation(uint16_t blockNum, uint32_t offset, uint32_t size,
	                             uint32_t writeId, uint32_t crc);

	/// @brief Returns the write operations to be performed by the JobPool workers.
	/// It merges the write operations if contiguous full blocks.
	/// The write operations are returned in the order they were received from the client.
	/// @return The vector of write operations.
	static std::vector<WriteOperation> getWriteOperations(
	    const std::vector<InputBuffer *> &inputBuffers, uint32_t &totalBlocks);

	/// @brief Returns the pointer to the block data and CRC for the given block index.
	/// If the block index is out of range, it returns nullptr and logs a warning.
	std::pair<const uint8_t *, size_t> getBlockDataAndCrc(uint16_t index) const;

	/// @brief Sets the statuses of the write operations to the provided buffers.
	/// @param inputBuffers The vector of input buffers to set the statuses for.
	/// @param statuses The vector of statuses to set.
	static void applyStatuses(const std::vector<InputBuffer *> &inputBuffers,
	                          std::vector<uint8_t> &statuses);

	/// @brief Returns the statuses,ID pair of the write operations in the buffer.
	/// @return The vector of statuses along with write IDs.
	std::vector<std::pair<uint8_t, uint32_t>> getStatuses() const;

	/// @brief Returns whether the header size is the expected one.
	bool isHeaderSizeValid() const;

	/// @brief Returns the write ID of the last write operation.
	uint32_t getLastWriteId() const;

	/// @brief Returns whether the buffer is full, i.e. has numBlocks_ write operations.
	bool isFull() const;

	/// @brief Returns whether the buffer is currently being updated (reading from socket).
	bool isBeingUpdated() const;

	/// @brief Number of blocks that have been replied to the client.
	std::atomic<uint16_t> repliedBlocks{0};

	/// @brief Returns the current number of blocks in the buffer.
	size_t currentBlocks() const { return writeInfo_.size(); }

	/// @brief Returns the vector of WriteInfo for the write operations in the buffer.
	std::vector<WriteInfo> &getWriteInfoVector() { return writeInfo_; }

	/// @brief Returns the pointer to the block data in the buffer for the given block index and
	/// offset in the block.
	const uint8_t *getBlockBufferData(size_t blockIndex, size_t offsetInBlock) const;

protected:
	const size_t headerSize_;  ///< The size of the header.
	const size_t numBlocks_;   ///< The number of blocks.

	bool isBeingUpdated_{false};

	/// The buffer for the block data.
	Buffer<std::vector<uint8_t, AlignedAllocator<uint8_t, disk::kIoBlockSize>>> blockBuffer_;
	/// The CRCs of the blocks to be written.
	std::vector<uint32_t> crcData_;
	/// The buffer for the header data.
	Buffer<std::vector<uint8_t>> headerBuffer_;
	/// The information about the blocks to be written.
	std::vector<WriteInfo> writeInfo_;
};

/// @brief The size of the header for the chunk-copy buffer.
/// The 0 is picked for simplicity, main goal is to make it a single value.
constexpr size_t kChunkCopyBufferHeaderSize = 0;

/**
 * @class ChunkCopyBuffer
 * @brief Wraps an IO aligned buffer to fit the BuffersPool interface, for the
 * paths that stage whole chunk blocks while copying them: replication and
 * local chunk duplication.
 *
 * The block buffer starts out empty; consumers size it to the batch they need,
 * and pooling keeps that capacity across uses.
 */
class ChunkCopyBuffer {
public:
	/// @brief Constructs a ChunkCopyBuffer with the given number of blocks.
	/// @param headerSize The size of the header (not used, but required for compatibility).
	/// @param numBlocks The number of blocks the buffer can hold.
	ChunkCopyBuffer(size_t headerSize, size_t numBlocks) : numBlocks_(numBlocks) {
		(void)headerSize;
		gCurrentTotalChunkCopyBufferBlocks += numBlocks_;
	}

	/// @brief Destructor only decreases the global counter of chunk-copy buffers blocks.
	~ChunkCopyBuffer() {
		gCurrentTotalChunkCopyBufferBlocks -= numBlocks_;
	}

	/// @brief Clears the buffer.
	void clear() { blockBuffer_.clear(); }

	/// @brief Returns the actual block buffer.
	std::vector<uint8_t, AlignedAllocator<uint8_t, disk::kIoBlockSize>> &getBlockBuffer() {
		return blockBuffer_;
	}

	/// @brief Returns the start of the block buffer.
	const uint8_t *data() const { return blockBuffer_.data(); }

	/// @brief Returns the type of the buffer.
	std::pair<size_t, size_t> type() const { return {kChunkCopyBufferHeaderSize, numBlocks_}; }

private:
	const size_t numBlocks_;  ///< The number of blocks.

	/// The buffer for the block data.
	std::vector<uint8_t, AlignedAllocator<uint8_t, disk::kIoBlockSize>> blockBuffer_{};
};

using OutputBufferPool = BuffersPool<OutputBuffer>;
using InputBufferPool = BuffersPool<InputBuffer>;
using ChunkCopyBufferPool = BuffersPool<ChunkCopyBuffer>;

/// @brief Returns the read output buffer pool.
/// It is a singleton.
inline OutputBufferPool &getReadOutputBufferPool() {
	static OutputBufferPool readOutputBuffersPool;
	return readOutputBuffersPool;
}

/// @brief Returns the write input buffer pool.
/// It is a singleton.
inline InputBufferPool &getWriteInputBufferPool() {
	static InputBufferPool writeInputBuffersPool;
	return writeInputBuffersPool;
}

/// @brief Returns the chunk-copy buffer pool, shared by replication and
/// local chunk duplication.
/// It is a singleton.
inline ChunkCopyBufferPool &getChunkCopyBuffersPool() {
	static ChunkCopyBufferPool chunkCopyBuffersPool;
	return chunkCopyBuffersPool;
}

/// @brief Releases the old IO buffers that have been in the pool for longer than the given
/// expiration time.
/// @param expirationTime_ms The expiration time in milliseconds.
void releaseOldIoBuffers(uint32_t expirationTime_ms);

/// @brief Sets the new maximum size of the IO buffers pool in megabytes.
/// @param maxBuffersPoolSize_mb The new maximum size in megabytes.
void setNewMaxIoBuffersPoolSize(size_t maxBuffersPoolSize_mb);

/// @brief Modifies the number of available write buffering blocks by the given value.
/// @param blocks The value to modify the number of available write buffering blocks by.
void modifyAvailableWriteBufferingBlocks(int32_t blocks);

/// @brief Returns the current number of available write buffering blocks.
/// This is the number of blocks that can be currently buffered for write operations, i.e. the
/// number of blocks that can be currently in the input buffers for write operations before they are
/// flushed to the disk.
int32_t getAvailableWriteBufferingBlocks();
