/*
   Copyright 2026 Leil Storage OÜ

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

#include "mock_disk.h"

#include <fcntl.h>
#include <algorithm>
#include <array>
#include <cstring>

#include "chunkserver-common/global_shared_resources.h"
#include "common/crc.h"
#include "common/datapack.h"
#include "common/slice_traits.h"
#include "errors/saunafs_error_codes.h"
#include "slogger/slogger.h"

namespace mockdisk {

MockDisk::MockDisk(const disk::Configuration &configuration, uint64_t chunkCount,
                   uint64_t firstChunkId)
    : CmrDisk(configuration), chunkCount_(chunkCount), firstChunkId_(firstChunkId) {}

const uint8_t *MockDisk::patternBlock() {
	static const auto block = []() {
		std::array<uint8_t, SFSBLOCKSIZE> data{};
		// Deterministic non-zero pattern, so tests can verify contents and
		// zero-block optimizations are never triggered.
		for (size_t i = 0; i < data.size(); ++i) {
			data[i] = static_cast<uint8_t>((i & 0xFFU) ^ 0x5AU);
		}
		return data;
	}();
	return block.data();
}

const uint8_t *MockDisk::patternBlockCrcBigEndian() {
	static const auto crc = []() {
		std::array<uint8_t, kCrcSize> serialized{};
		uint8_t *ptr = serialized.data();
		put32bit(&ptr, mycrc32(0, patternBlock(), SFSBLOCKSIZE));
		return serialized;
	}();
	return crc.data();
}

bool MockDisk::scanSyntheticChunks(const SyntheticChunkSink &sink) {
	sink.reserve(chunkCount_);

	const auto chunkType = slice_traits::standard::ChunkPartType();
	std::vector<ChunkWithVersionAndType> bulk;
	uint64_t emitted = 0;
	uint8_t lastReportedPercent = 0;

	while (emitted < chunkCount_) {
		if (sink.isTerminating()) { return true; }

		const uint64_t bulkSize = std::min(kScanBulkSize, chunkCount_ - emitted);
		bulk.clear();
		bulk.reserve(bulkSize);

		for (uint64_t i = 0; i < bulkSize; ++i) {
			bulk.emplace_back(firstChunkId_ + emitted + i, kMockChunkVersion, chunkType);
		}

		sink.emitBulk(std::move(bulk));
		emitted += bulkSize;

		const auto percent = static_cast<uint8_t>((emitted * 100) / chunkCount_);
		if (percent > lastReportedPercent) {
			lastReportedPercent = percent;
			std::lock_guard disksLockGuard(gDisksMutex);
			setScanProgress(percent);
		}
	}

	safs::log_info("mock disk {}: synthesized {} chunks (first id {})", metaPath(), chunkCount_,
	               firstChunkId_);

	return true;
}

void MockDisk::refreshDataDiskUsage() {
	// One block per chunk: plausible sizes even at hundreds of millions of
	// chunks. availableSpace == 0 keeps space heuristics away.
	setTotalSpace(chunkCount_ * SFSBLOCKSIZE);
	setAvailableSpace(0ULL);
}

int MockDisk::updateChunkAttributes(IChunk *chunk, bool /*isFromScan*/) {
	chunk->setBlocks(chunk->maxBlocksInFile());
	chunk->setValidAttr(1);
	return SAUNAFS_STATUS_OK;
}

void MockDisk::creat(IChunk *chunk) { open(chunk); }

void MockDisk::open(IChunk *chunk) {
	// Real descriptors are required: they key the shared crc/open-file
	// resources (gOpenChunks). /dev/null accepts both reads and writes.
	chunk->setMetaFD(::open("/dev/null", O_RDWR));
	chunk->setDataFD(::open("/dev/null", O_RDWR));
}

int MockDisk::unlinkChunk(IChunk * /*chunk*/) { return SAUNAFS_STATUS_OK; }

int MockDisk::ftruncateData(IChunk * /*chunk*/, uint64_t /*size*/) { return 0; }

int MockDisk::readChunkCrc(IChunk *chunk, uint32_t /*chunkVersion*/, uint8_t *crcData) {
	const size_t crcBlockSize = chunk->getCrcBlockSize();
	for (size_t offset = 0; offset < crcBlockSize; offset += kCrcSize) {
		memcpy(crcData + offset, patternBlockCrcBigEndian(), kCrcSize);
	}
	return SAUNAFS_STATUS_OK;
}

int MockDisk::readBlockAndCrc(IChunk * /*chunk*/, uint8_t *blockBuffer, uint8_t * /*crcData*/,
                              uint16_t /*blocknum*/, const char * /*errorMsg*/) {
	memcpy(blockBuffer, patternBlockCrcBigEndian(), kCrcSize);
	memcpy(blockBuffer + kCrcSize, patternBlock(), SFSBLOCKSIZE);
	return SFSBLOCKSIZE;
}

ssize_t MockDisk::preadData(IChunk * /*chunk*/, uint8_t *blockBuffer, uint64_t size,
                            uint64_t offset) {
	// Every block contains the same pattern; copy with block alignment.
	uint64_t copied = 0;
	while (copied < size) {
		const uint64_t offsetInBlock = (offset + copied) % SFSBLOCKSIZE;
		const uint64_t toCopy = std::min<uint64_t>(size - copied, SFSBLOCKSIZE - offsetInBlock);
		memcpy(blockBuffer + copied, patternBlock() + offsetInBlock, toCopy);
		copied += toCopy;
	}
	return static_cast<ssize_t>(size);
}

void MockDisk::prefetchChunkBlocks(IChunk & /*chunk*/, uint16_t /*firstBlock*/,
                                   uint32_t /*blockCount*/) {}

int MockDisk::overwriteChunkVersion(IChunk *chunk, uint32_t newVersion) {
	chunk->setVersion(newVersion);
	return SAUNAFS_STATUS_OK;
}

int MockDisk::writePartialBlockAndCrc(IChunk * /*chunk*/, const uint8_t * /*buffer*/,
                                      uint32_t /*offsetInBlock*/, uint32_t size,
                                      const uint8_t *crcBuff, uint8_t *crcData, uint16_t blockNum,
                                      bool /*isNewBlock*/, const char * /*errorMsg*/) {
	memcpy(crcData + blockNum * kCrcSize, crcBuff, kCrcSize);
	return static_cast<int>(size);
}

int MockDisk::writeFullBlocksAndCrcs(IChunk * /*chunk*/,
                                     const std::vector<uint16_t> &blocksPerBuffer,
                                     const std::vector<const uint8_t *> & /*buffers*/,
                                     uint16_t startBlock, const uint8_t *crcBuff, uint8_t *crcData,
                                     bool /*areNewBlocks*/, const char * /*errorMsg*/) {
	uint32_t blocksWritten = 0;
	for (const auto blocksInBuffer : blocksPerBuffer) { blocksWritten += blocksInBuffer; }
	memcpy(crcData + startBlock * kCrcSize, crcBuff, kCrcSize * blocksWritten);
	return static_cast<int>(blocksWritten * SFSBLOCKSIZE);
}

}  // namespace mockdisk
