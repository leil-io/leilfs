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
#include <unistd.h>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <random>

#include "chunkserver/io_buffers.h"
#include "common/crc.h"
#include "errors/saunafs_error_codes.h"

const ssize_t testHeaderSize = 1;
const ssize_t testNumBlocks = 4;
const ssize_t testPipePageSize = 512 * 1024;

// OutputBuffer tests

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

class InputBufferTests : public InputBuffer {
public:
	InputBufferTests() : InputBuffer(testHeaderSize, testNumBlocks) {}
};

void checkWriteOperation(std::vector<WriteOperation> &writeOperations,
                         std::vector<WriteOperation> &expectedWriteOperations) {
	ASSERT_EQ(writeOperations.size(), expectedWriteOperations.size());
	for (size_t i = 0; i < writeOperations.size(); i++) {
		ASSERT_EQ(writeOperations[i].startBlock, expectedWriteOperations[i].startBlock);
		ASSERT_EQ(writeOperations[i].blocksPerBuffer, expectedWriteOperations[i].blocksPerBuffer);
		ASSERT_EQ(writeOperations[i].offset, expectedWriteOperations[i].offset);
		ASSERT_EQ(writeOperations[i].size, expectedWriteOperations[i].size);
		ASSERT_EQ(writeOperations[i].buffers, expectedWriteOperations[i].buffers);
		ASSERT_EQ(writeOperations[i].crcs, expectedWriteOperations[i].crcs);
	}
}

// InputBuffer tests

TEST(InputBufferTests, inputBufferBasicWriteToSocketTest) {
	InputBufferTests inputBuffer;

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
	const unsigned writeSize = writeSizeData + testHeaderSize;
	const unsigned value = 17u;

	std::vector<uint8_t> buf(writeSize, value);
	inputBuffer.addNewWriteOperation();
	ASSERT_EQ(
	    inputBuffer.copyIntoBuffer(InputBuffer::BufferType::Header, buf.data(), testHeaderSize),
	    testHeaderSize);
	ASSERT_EQ(inputBuffer.copyIntoBuffer(InputBuffer::BufferType::Block, buf.data(), writeSizeData),
	          writeSizeData);

	int totalBytesWritten = 0;
	while (true) {
		auto bytesWritten =
		    inputBuffer.writeToSocket(auxPipeFileDescriptors[1], writeSize - totalBytesWritten);
		ASSERT_GE(bytesWritten, 0);
		totalBytesWritten += bytesWritten;
		if (totalBytesWritten == writeSize) { break; }
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

TEST(InputBufferTests, inputBufferBasicReadFromSocketTest) {
	InputBufferTests inputBuffer;

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
	const unsigned writeSize = writeSizeData + testHeaderSize;
	const unsigned value = 17u;

	std::vector<uint8_t> buf(writeSize, value);
	ASSERT_EQ(write(auxPipeFileDescriptors[1], buf.data(), writeSize), writeSize);
	inputBuffer.addNewWriteOperation();

	int totalBytesRead = 0;
	while (true) {
		auto bytesRead =
		    inputBuffer.readFromSocket(auxPipeFileDescriptors[0], writeSize - totalBytesRead);
		ASSERT_GE(bytesRead, 0);
		totalBytesRead += bytesRead;
		if (totalBytesRead == writeSize) { break; }
		sleep(1);
	}

	for (uint32_t j = 0; j < testHeaderSize; ++j) {
		ASSERT_EQ(value, inputBuffer.rawData(InputBuffer::BufferType::Header)[j])
		    << "Byte " << j << " in block doesn't match";
	}
	for (uint32_t j = 0; j < writeSizeData; ++j) {
		ASSERT_EQ(value, inputBuffer.rawData(InputBuffer::BufferType::Block)[j])
		    << "Byte " << j << " in block doesn't match";
	}

	close(auxPipeFileDescriptors[0]);
	close(auxPipeFileDescriptors[1]);
}

TEST(InputBufferTests, inputBufferAddSetupGetWriteOperationsSingleBufferTest) {
	InputBufferTests inputBuffer;
	std::vector<InputBuffer *> inputBuffers{&inputBuffer};
	uint32_t totalBlocks = 12345;
	constexpr uint32_t dummyWriteId = 0;

	// Test contiguous full blocks
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(0, 0, SFSBLOCKSIZE, dummyWriteId, 1);
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(1, 0, SFSBLOCKSIZE, dummyWriteId, 666);
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(2, 0, SFSBLOCKSIZE, dummyWriteId, 555);
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(3, 0, SFSBLOCKSIZE, dummyWriteId, 444);
	std::vector<WriteOperation> writeOperations =
	    InputBuffer::getWriteOperations(inputBuffers, totalBlocks);
	std::vector<WriteOperation> expectedWriteOperations = {
	    {.startBlock = 0,
	     .blocksPerBuffer = {4},
	     .buffers = {inputBuffer.rawData(InputBuffer::BufferType::Block)},
	     .offset = 0,
	     .size = SFSBLOCKSIZE,
	     .crcs = {1, 666, 555, 444}}};
	checkWriteOperation(writeOperations, expectedWriteOperations);
	ASSERT_EQ(totalBlocks, 4u);

	// Test non-contiguous blocks and non-full block
	inputBuffer.clear();
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(5, 0, SFSBLOCKSIZE, dummyWriteId, 123);
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(7, 0, SFSBLOCKSIZE, dummyWriteId, 456);
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(8, 0, 12345, dummyWriteId, 789);
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(9, 0, SFSBLOCKSIZE, dummyWriteId, 901);
	totalBlocks = 12345;
	writeOperations = InputBuffer::getWriteOperations(inputBuffers, totalBlocks);
	expectedWriteOperations = {
	    {.startBlock = 5,
	     .blocksPerBuffer = {1},
	     .buffers = {inputBuffer.rawData(InputBuffer::BufferType::Block)},
	     .offset = 0,
	     .size = SFSBLOCKSIZE,
	     .crcs = {123}},
	    {.startBlock = 7,
	     .blocksPerBuffer = {1},
	     .buffers = {inputBuffer.rawData(InputBuffer::BufferType::Block) + SFSBLOCKSIZE},
	     .offset = 0,
	     .size = SFSBLOCKSIZE,
	     .crcs = {456}},
	    {.startBlock = 8,
	     .blocksPerBuffer = {1},
	     .buffers = {inputBuffer.rawData(InputBuffer::BufferType::Block) + 2 * SFSBLOCKSIZE},
	     .offset = 0,
	     .size = 12345,
	     .crcs = {789}},
	    {.startBlock = 9,
	     .blocksPerBuffer = {1},
	     .buffers = {inputBuffer.rawData(InputBuffer::BufferType::Block) + 3 * SFSBLOCKSIZE},
	     .offset = 0,
	     .size = SFSBLOCKSIZE,
	     .crcs = {901}}};
	checkWriteOperation(writeOperations, expectedWriteOperations);
	ASSERT_EQ(totalBlocks, 4u);

	// Test apparently contiguous data which start in non-full block, and test block overwrite
	inputBuffer.clear();
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(10, 12345, SFSBLOCKSIZE - 12345, dummyWriteId, 111);
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(11, 0, SFSBLOCKSIZE, dummyWriteId, 222);
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(12, 0, SFSBLOCKSIZE, dummyWriteId, 333);
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(12, 0, SFSBLOCKSIZE, dummyWriteId, 444);
	totalBlocks = 12345;
	writeOperations = InputBuffer::getWriteOperations(inputBuffers, totalBlocks);
	expectedWriteOperations = {
	    {.startBlock = 10,
	     .blocksPerBuffer = {1},
	     .buffers = {inputBuffer.rawData(InputBuffer::BufferType::Block)},
	     .offset = 12345,
	     .size = SFSBLOCKSIZE - 12345,
	     .crcs = {111}},
	    {.startBlock = 11,
	     .blocksPerBuffer = {2},
	     .buffers = {inputBuffer.rawData(InputBuffer::BufferType::Block) + SFSBLOCKSIZE},
	     .offset = 0,
	     .size = SFSBLOCKSIZE,
	     .crcs = {222, 333}},
	    {.startBlock = 12,
	     .blocksPerBuffer = {1},
	     .buffers = {inputBuffer.rawData(InputBuffer::BufferType::Block) + 3 * SFSBLOCKSIZE},
	     .offset = 0,
	     .size = SFSBLOCKSIZE,
	     .crcs = {444}}};
	checkWriteOperation(writeOperations, expectedWriteOperations);
	ASSERT_EQ(totalBlocks, 4u);

	// Test extreme block indexes, last before SFSBLOCKSINCHUNK, 0 and a couple of contiguous full
	// blocks far beyond the current SFSBLOCKSINCHUNK, which are mergeable.
	inputBuffer.clear();
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(1023, 12345, SFSBLOCKSIZE, dummyWriteId, 555);
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(0, 0, 12345, dummyWriteId, 666);
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(9999, 0, SFSBLOCKSIZE, dummyWriteId, 777);
	inputBuffer.addNewWriteOperation();
	inputBuffer.setupLastWriteOperation(10000, 0, SFSBLOCKSIZE, dummyWriteId, 888);
	totalBlocks = 12345;
	writeOperations = InputBuffer::getWriteOperations(inputBuffers, totalBlocks);
	expectedWriteOperations = {
	    {.startBlock = 1023,
	     .blocksPerBuffer = {1},
	     .buffers = {inputBuffer.rawData(InputBuffer::BufferType::Block)},
	     .offset = 12345,
	     .size = SFSBLOCKSIZE,
	     .crcs = {555}},
	    {.startBlock = 0,
	     .blocksPerBuffer = {1},
	     .buffers = {inputBuffer.rawData(InputBuffer::BufferType::Block) + SFSBLOCKSIZE},
	     .offset = 0,
	     .size = 12345,
	     .crcs = {666}},
	    {.startBlock = 9999,
	     .blocksPerBuffer = {2},
	     .buffers = {inputBuffer.rawData(InputBuffer::BufferType::Block) + 2 * SFSBLOCKSIZE},
	     .offset = 0,
	     .size = SFSBLOCKSIZE,
	     .crcs = {777, 888}}};
	checkWriteOperation(writeOperations, expectedWriteOperations);
	ASSERT_EQ(totalBlocks, 4u);

	std::vector<uint8_t> statuses = {SAUNAFS_STATUS_OK, SAUNAFS_ERROR_WRONGSIZE, SAUNAFS_ERROR_IO,
	                                 SAUNAFS_ERROR_IO};
	InputBuffer::applyStatuses({&inputBuffer}, statuses);
	auto statusesWriteIdPairs = inputBuffer.getStatuses();

	std::vector<std::pair<uint8_t, uint32_t>> expectedStatuses = {
	    {SAUNAFS_STATUS_OK, dummyWriteId},
	    {SAUNAFS_ERROR_WRONGSIZE, dummyWriteId},
	    {SAUNAFS_ERROR_IO, dummyWriteId},
	    {SAUNAFS_ERROR_IO, dummyWriteId}};
	ASSERT_EQ(statusesWriteIdPairs, expectedStatuses);
}

TEST(InputBufferTests, inputBufferAddSetupGetWriteOperationsMultiBufferTest) {
	constexpr uint32_t dummyWriteId = 0;
	InputBufferTests inputBuffer1;  // blocks 0,1,2,3
	InputBufferTests inputBuffer2;  // blocks 4,5,6,7
	InputBufferTests inputBuffer3;  // blocks 8,9,10,11
	InputBufferTests inputBuffer4;  // blocks 12, part of 13
	std::vector<InputBuffer *> inputBuffers{&inputBuffer1, &inputBuffer2, &inputBuffer3,
	                                        &inputBuffer4};
	std::vector<uint32_t> crcs;
	uint32_t totalBlocks = 12345;

	// Test contiguous blocks from multiple buffers, and split the last input buffer
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++) {
			uint32_t blockIndex = j + 4 * i;
			inputBuffers[i]->addNewWriteOperation();
			inputBuffers[i]->setupLastWriteOperation(blockIndex, 0, SFSBLOCKSIZE, dummyWriteId,
			                                         blockIndex * 111);
			crcs.push_back(blockIndex * 111);
		}
	}
	inputBuffer4.addNewWriteOperation();
	inputBuffer4.setupLastWriteOperation(12, 0, SFSBLOCKSIZE, dummyWriteId, 12 * 111);
	crcs.push_back(12 * 111);
	inputBuffer4.addNewWriteOperation();
	inputBuffer4.setupLastWriteOperation(13, 0, 12345, dummyWriteId, 13 * 111);

	std::vector<WriteOperation> writeOperations =
	    InputBuffer::getWriteOperations(inputBuffers, totalBlocks);
	std::vector<WriteOperation> expectedWriteOperations = {
	    {.startBlock = 0,
	     .blocksPerBuffer = {4, 4, 4, 1},
	     .buffers = {inputBuffer1.rawData(InputBuffer::BufferType::Block),
	                 inputBuffer2.rawData(InputBuffer::BufferType::Block),
	                 inputBuffer3.rawData(InputBuffer::BufferType::Block),
	                 inputBuffer4.rawData(InputBuffer::BufferType::Block)},
	     .offset = 0,
	     .size = SFSBLOCKSIZE,
	     .crcs = crcs},
	    {.startBlock = 13,
	     .blocksPerBuffer = {1},
	     .buffers = {inputBuffer4.rawData(InputBuffer::BufferType::Block) + SFSBLOCKSIZE},
	     .offset = 0,
	     .size = 12345,
	     .crcs = {13 * 111}}};
	checkWriteOperation(writeOperations, expectedWriteOperations);
	ASSERT_EQ(totalBlocks, 14u);
}

TEST(InputBufferTests, inputBufferGeneralTest) {
	InputBufferTests inputBuffer;

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
	std::vector<uint16_t> offsets = {0, 1024, 0, 0};
	std::vector<int> dataSizes = {12345, 1024, SFSBLOCKSIZE, SFSBLOCKSIZE};
	std::vector<uint8_t> buf;
	for (auto &dataSize : dataSizes) {
		for (int i = 0; i <= dataSize; i++) {
			uint64_t randomValue = rng();
			buf.push_back(static_cast<uint8_t>(randomValue & 0xFF));
		}
	}

	ASSERT_EQ(write(auxPipeFileDescriptors[1], buf.data(), buf.size()), (int)buf.size());

	std::mutex mtx;
	std::condition_variable cv;
	std::atomic<bool> consumerThreadIsWaiting = false;

	// Consumer thread
	auto consumerThread = std::thread([&]() {
		{
			std::unique_lock lock(mtx);
			consumerThreadIsWaiting = true;
			cv.wait(lock);
			consumerThreadIsWaiting = false;
		}

		std::vector<uint8_t> statuses(dataSizes.size(), SAUNAFS_ERROR_IO);
		InputBuffer::applyStatuses({&inputBuffer}, statuses);
	});

	// Make sure the consumer thread is waiting for the wake up call
	while (!consumerThreadIsWaiting) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }

	for (size_t i = 0; i < dataSizes.size(); ++i) {
		inputBuffer.addNewWriteOperation();
		inputBuffer.readFromSocket(auxPipeFileDescriptors[1], dataSizes[i] + 1);
		inputBuffer.setupLastWriteOperation(i, offsets[i], dataSizes[i], i + 1, i + 1);
		ASSERT_EQ(consumerThreadIsWaiting.load(), true);

		if (i == dataSizes.size() - 1) {
			// Last operation, let's wake up the consumer thread
			std::unique_lock lock(mtx);
			cv.notify_all();
			lock.unlock();
		}
	}

	auto statuses = inputBuffer.getStatuses();
	for (uint32_t tryCount = 0; tryCount < 10; tryCount++) {
		statuses = inputBuffer.getStatuses();
		auto it = std::find_if(statuses.begin(), statuses.end(), [](auto statusIDPair) {
			return statusIDPair.first != static_cast<uint8_t>(SAUNAFS_ERROR_IO);
		});
		if (it == statuses.end()) { break; }

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	for (const auto &[status, _] : statuses) { ASSERT_EQ(SAUNAFS_ERROR_IO, status); }

	if (consumerThread.joinable()) { consumerThread.join(); }

	close(auxPipeFileDescriptors[0]);
	close(auxPipeFileDescriptors[1]);
}
