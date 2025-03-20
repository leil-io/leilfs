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
		Done,   // The write operation was successful.
		Again,  // The write operation should be retried.
		Error   // An error occurred during the write operation.
	};

	/// @enum BufferType
	/// @brief Represents the type of buffer.
	enum class BufferType : uint8_t {
		Block,  // The block buffer.
		CRC,    // The CRC buffer.
		Header  // The header buffer.
	};

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
				ssize_t ret = chunk->owner()->preadData(
				    chunk, &data_[unflushedDataOneAfterLastIndex_], len, offset);
				if (ret <= 0) { return bytes_written; }
				len -= ret;
				unflushedDataOneAfterLastIndex_ += ret;
				bytes_written += ret;
			}
			return bytes_written;
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
		inline void moveUnflushedDataFirstIndex(int64_t offset) {
			unflushedDataFirstIndex_ += offset;
		}

	private:
		const size_t capacity_;      // The capacity of the buffer.
		const size_t trueCapacity_;  // The real size of the underlying container.
		const size_t padding_;       // The amount of unused space at the start of the container.
		size_t unflushedDataFirstIndex_;         // The index of the first unflushed data.
		size_t unflushedDataOneAfterLastIndex_;  // The index of the first byte after the last
		                                         // unflushed data.
		ContainerType data_;                     // The underlying data container.
	};

	/// @brief Constructs an OutputBuffer with the given header size and number of blocks.
	/// @param headerSize The size of the header.
	/// @param numBlocks The number of blocks.
	explicit OutputBuffer(size_t headerSize, size_t numBlocks);

	/// @brief Default destructor.
	~OutputBuffer() = default;

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

	/// Status of the buffer's related read operation.
	std::atomic_uint8_t status{kNotSaunafsStatus};

private:
	// The current remaining bytes to be written to the file descriptor at once.
	// When the buffer is prepared, should be equal to: SFSBLOCKSIZE + kCrcSize + headerSize_.
	// 0 means that the buffer is not prepared or is finished.
	size_t currentRemainingBytesForFD_;
	// The size of the header.
	const size_t headerSize_;
	// The number of blocks.
	const size_t numBlocks_;

	// The buffer for the block data.
	Buffer<std::vector<uint8_t, AlignedAllocator<uint8_t, disk::kIoBlockSize>>> blockBuffer_;
	// The buffer for the CRC data.
	Buffer<std::vector<uint8_t>> crcBuffer_;
	// The buffer for the header data.
	Buffer<std::vector<uint8_t>> headerBuffer_;
};

using OutputBufferPool = BuffersPool<OutputBuffer>;

/// @brief Returns the read output buffer pool.
/// It is a singleton.
inline OutputBufferPool &getReadOutputBufferPool() {
	static OutputBufferPool readOutputBuffersPool;
	return readOutputBuffersPool;
}
