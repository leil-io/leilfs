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

#include <fcntl.h>
#include <gtest/gtest.h>
#include <cstdlib>
#include <random>

#include "chunkserver/io_buffers.h"
#include "common/crc.h"

const ssize_t testHeaderSize = 1;
const ssize_t testNumBlocks = 4;
const ssize_t testPipePageSize = 512 * 1024;

TEST(OutputBufferTests, outputBufferBasicTest) {
	OutputBuffer outputBuffer(testHeaderSize, testNumBlocks);

	int auxPipeFileDescriptors[2];

#if defined(__APPLE__)
	ASSERT_NE(pipe(auxPipeFileDescriptors), -1);
#else
	ASSERT_NE(pipe2(auxPipeFileDescriptors, O_NONBLOCK), -1);
#endif

#ifdef F_SETPIPE_SZ
	ASSERT_NE(fcntl(auxPipeFileDescriptors[1], F_SETPIPE_SZ, testPipePageSize), -1);
#endif

	const unsigned writeSizeData = 10;
	const unsigned writeSize = writeSizeData + kCrcSize + testHeaderSize;
	const unsigned value = 17u;

	std::vector<uint8_t> buf(writeSize, value);
	ASSERT_EQ(outputBuffer.copyIntoBuffer(OutputBuffer::BufferType::Header,
	                                      std::vector<uint8_t>(testHeaderSize, value)),
	          testHeaderSize);
	ASSERT_EQ(outputBuffer.copyIntoBuffer(OutputBuffer::BufferType::CRC, buf.data(), kCrcSize),
	          kCrcSize);
	ASSERT_EQ(
	    outputBuffer.copyIntoBuffer(OutputBuffer::BufferType::Block, buf.data(), writeSizeData),
	    writeSizeData);

	while (true) {
		OutputBuffer::WriteStatus status =
		    outputBuffer.writeOutToAFileDescriptor(auxPipeFileDescriptors[1]);
		ASSERT_NE(status, OutputBuffer::WriteStatus::Error);
		if (status == OutputBuffer::WriteStatus::Done) { break; }
		sleep(1);
	}

	ASSERT_EQ(read(auxPipeFileDescriptors[0], buf.data(), writeSize), writeSize)
	    << "errno: " << errno;

	for (uint32_t j = 0; j < writeSize; ++j) {
		ASSERT_EQ(value, buf[j]) << "Byte " << j << " in block doesn't match";
	}
	close(auxPipeFileDescriptors[0]);
	close(auxPipeFileDescriptors[1]);
}

TEST(OutputBufferTests, outputBufferCheckCrcTest) {
	OutputBuffer outputBuffer(testHeaderSize, testNumBlocks);

	std::mt19937_64 rng(time(0));
	std::vector<uint32_t> blocksCrc(testNumBlocks);
	std::vector<uint8_t> buf(SFSBLOCKSIZE);
	// Put testNumBlocks blocks of random data into the buffer
	for (uint32_t blockNumber = 0; blockNumber < testNumBlocks; blockNumber++) {
		for (uint32_t i = 0; i < SFSBLOCKSIZE;) {
			uint64_t randomValue = rng();
			memcpy(buf.data() + i, &randomValue, sizeof(randomValue));
			i += sizeof(randomValue);
		}
		blocksCrc[blockNumber] = mycrc32(0, buf.data(), SFSBLOCKSIZE);
		ASSERT_EQ(
		    outputBuffer.copyIntoBuffer(OutputBuffer::BufferType::Block, buf.data(), SFSBLOCKSIZE),
		    SFSBLOCKSIZE)
		    << "Failed to copy block " << blockNumber << " into the buffer";
	}

	// Randomize the order of blocks to check the CRC
	std::vector<uint32_t> orderToCheckCrc;
	for (uint32_t i = 0; i < testNumBlocks; i++) { orderToCheckCrc.push_back(i); }
	std::shuffle(orderToCheckCrc.begin(), orderToCheckCrc.end(), rng);

	// Check CRC for the blocks in the randomized order
	for (auto blockNumber : orderToCheckCrc) {
		ASSERT_TRUE(
		    outputBuffer.checkCRC(SFSBLOCKSIZE, blocksCrc[blockNumber], blockNumber * SFSBLOCKSIZE))
		    << "CRC check failed for block " << blockNumber;
	}
}

TEST(OutputBufferTests, outputBufferCopyValueTest) {
	OutputBuffer outputBuffer(testHeaderSize, testNumBlocks);

	std::vector<uint8_t> buf(SFSBLOCKSIZE, 0);
	const auto emptyBlockCrc = mycrc32(0, buf.data(), SFSBLOCKSIZE);

	// Put testNumBlocks blocks of zeroes into the buffer
	for (uint32_t blockNumber = 0; blockNumber < testNumBlocks; blockNumber++) {
		ASSERT_EQ(
		    outputBuffer.copyValueIntoBuffer(OutputBuffer::BufferType::Block, 0, SFSBLOCKSIZE),
		    SFSBLOCKSIZE)
		    << "Failed to copy zeroes block " << blockNumber << " into the buffer";
	}

	// Randomize the order of blocks to check the CRC
	std::vector<uint32_t> orderToCheckCrc;
	for (uint32_t i = 0; i < testNumBlocks; i++) { orderToCheckCrc.push_back(i); }
	std::shuffle(orderToCheckCrc.begin(), orderToCheckCrc.end(), std::mt19937_64(time(0)));

	// Check CRC for the blocks in the randomized order
	for (auto blockNumber : orderToCheckCrc) {
		ASSERT_TRUE(outputBuffer.checkCRC(SFSBLOCKSIZE, emptyBlockCrc, blockNumber * SFSBLOCKSIZE))
		    << "CRC check failed for block " << blockNumber;
	}
}

TEST(OutputBufferTests, outputBufferSeveralBlocksInBufferTest) {
	OutputBuffer outputBuffer(testHeaderSize, testNumBlocks);

	int auxPipeFileDescriptors[2];

#if defined(__APPLE__)
	ASSERT_NE(pipe(auxPipeFileDescriptors), -1);
#else
	ASSERT_NE(pipe2(auxPipeFileDescriptors, O_NONBLOCK), -1);
#endif

#ifdef F_SETPIPE_SZ
	ASSERT_NE(fcntl(auxPipeFileDescriptors[1], F_SETPIPE_SZ, testPipePageSize), -1);
#endif

	std::mt19937_64 rng(time(0));
	std::vector<uint8_t> buf(SFSBLOCKSIZE);

	// Put testNumBlocks - 1 blocks of random data into the buffer
	std::vector<uint32_t> blockDataSizesLog2;
	for (uint32_t i = 0; i < testNumBlocks - 1; i++) { blockDataSizesLog2.push_back(16); }
	// The last block is smaller
	uint32_t lastBlockSizeLog2 = 10;
	blockDataSizesLog2.push_back(lastBlockSizeLog2);

	for (uint32_t blockNumber = 0; blockNumber < testNumBlocks; blockNumber++) {
		uint8_t headerValue = blockDataSizesLog2[blockNumber];
		uint32_t blockSize = 1 << headerValue;
		for (uint32_t i = 0; i < blockSize;) {
			uint64_t randomValue = rng();
			memcpy(buf.data() + i, &randomValue, sizeof(randomValue));
			i += sizeof(randomValue);
		}
		ASSERT_EQ(outputBuffer.copyIntoBuffer(OutputBuffer::BufferType::Header,
		                                      std::vector<uint8_t>(testHeaderSize, headerValue)),
		          testHeaderSize);
		auto crc = mycrc32(0, buf.data(), blockSize);
		ASSERT_EQ(outputBuffer.copyIntoBuffer(OutputBuffer::BufferType::CRC, &crc, kCrcSize),
		          kCrcSize);
		ASSERT_EQ(
		    outputBuffer.copyIntoBuffer(OutputBuffer::BufferType::Block, buf.data(), blockSize),
		    blockSize);
	}

	while (true) {
		OutputBuffer::WriteStatus status =
		    outputBuffer.writeOutToAFileDescriptor(auxPipeFileDescriptors[1]);
		ASSERT_NE(status, OutputBuffer::WriteStatus::Error);
		if (status == OutputBuffer::WriteStatus::Done) { break; }
		sleep(1);
	}

	uint32_t expectedSize = testNumBlocks * kCrcSize + testNumBlocks * testHeaderSize;
	for (uint32_t blockSizeLog2 : blockDataSizesLog2) { expectedSize += 1u << blockSizeLog2; }
	buf.resize(expectedSize);
	buf.reserve(expectedSize);

	ASSERT_EQ(read(auxPipeFileDescriptors[0], buf.data(), expectedSize), expectedSize)
	    << "errno: " << errno;

	// Check the actually written data
	uint32_t offset = 0;
	for (uint32_t j = 0; j < testNumBlocks; ++j) {
		// Get the header value
		auto headerValue = buf[offset];
		offset += testHeaderSize;

		// Get the CRC
		auto crc = *reinterpret_cast<const uint32_t *>(buf.data() + offset);
		offset += kCrcSize;

		// Remember the block size was 2^headerValue
		auto blockSize = 1u << headerValue;

		// Check the block data matches the CRC
		ASSERT_EQ(mycrc32(0, buf.data() + offset, blockSize), crc)
		    << "CRC check failed for block " << j;
		offset += blockSize;
	}
	close(auxPipeFileDescriptors[0]);
	close(auxPipeFileDescriptors[1]);
}
