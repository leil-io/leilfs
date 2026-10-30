/*
   Copyright 2026 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <gtest/gtest.h>
#include <sys/poll.h>

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "chunkserver-common/cmr_chunk.h"
#include "chunkserver-common/cmr_disk.h"
#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/hdd_utils.h"
#include "chunkserver/bgjobs.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/io_buffers.h"
#include "common/crc.h"
#include "common/datapack.h"
#include "errors/leil_error_codes.h"
#include "protocol/cltocs.h"

namespace {

class NewChunkReportQueueTests : public testing::Test {
protected:
	void SetUp() override {
		hddDiscardNewChunks();
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		ASSERT_TRUE(gChunksMap.empty());
		ASSERT_TRUE(gPresentChunkTypes.empty());
	}
	void TearDown() override {
		hddDiscardNewChunks();
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		gChunksMap.clear();
		gPresentChunkTypes.clear();
	}

	void enqueue(uint64_t id, bool fromDiskScan) {
		const auto type = ChunkPartType();
		{
			std::lock_guard chunksMapLockGuard(gChunksMapMutex);
			if (!gChunksMap.contains(makeChunkKey(id, type))) {
				gChunksMap.emplace(makeChunkKey(id, type),
				                   std::make_unique<CmrChunk>(id, type, ChunkState::Available));
				hddNotePresentChunkType(type);
			}
		}
		hddEnqueueChunkReport(ChunkWithVersionAndType(id, 1, type), fromDiskScan);
	}
};

TEST_F(NewChunkReportQueueTests, PreservesOriginAcrossSeparateBatches) {
	enqueue(1, false);
	enqueue(2, true);
	enqueue(3, false);

	std::vector<ChunkWithVersionAndType> chunks;
	bool fromScan = true;
	hddGetNewChunks(chunks, 1, &fromScan);
	ASSERT_EQ(chunks.size(), 1U);
	EXPECT_EQ(chunks.front().id, 1U);
	EXPECT_FALSE(fromScan);

	hddGetNewChunks(chunks, 1, &fromScan);
	ASSERT_EQ(chunks.size(), 1U);
	EXPECT_EQ(chunks.front().id, 2U);
	EXPECT_TRUE(fromScan);

	hddGetNewChunks(chunks, 1, &fromScan);
	ASSERT_EQ(chunks.size(), 1U);
	EXPECT_EQ(chunks.front().id, 3U);
	EXPECT_FALSE(fromScan);
}

TEST_F(NewChunkReportQueueTests, TreatsMixedBatchAsRegistration) {
	enqueue(1, false);
	enqueue(2, true);

	std::vector<ChunkWithVersionAndType> chunks;
	bool fromScan = false;
	hddGetNewChunks(chunks, 2, &fromScan);

	ASSERT_EQ(chunks.size(), 2U);
	EXPECT_EQ(chunks[0].id, 1U);
	EXPECT_EQ(chunks[1].id, 2U);
	EXPECT_TRUE(fromScan);
}

TEST_F(NewChunkReportQueueTests, DistinguishesDuplicateReportsByOrigin) {
	enqueue(1, false);
	enqueue(1, true);

	std::vector<ChunkWithVersionAndType> chunks;
	bool fromScan = true;
	hddGetNewChunks(chunks, 1, &fromScan);
	EXPECT_FALSE(fromScan);

	hddGetNewChunks(chunks, 1, &fromScan);
	EXPECT_TRUE(fromScan);
}

TEST_F(NewChunkReportQueueTests, CoalescesAndPartiallyConsumesRuns) {
	enqueue(1, true);
	enqueue(2, true);
	enqueue(3, false);
	ASSERT_EQ(gNewChunkReportOriginRuns.size(), 2U);
	EXPECT_EQ(gNewChunkReportOriginRuns.front().count, 2U);

	std::vector<ChunkWithVersionAndType> chunks;
	bool fromScan = false;
	hddGetNewChunks(chunks, 1, &fromScan);
	EXPECT_TRUE(fromScan);
	ASSERT_EQ(gNewChunkReportOriginRuns.size(), 2U);
	EXPECT_EQ(gNewChunkReportOriginRuns.front().count, 1U);

	hddGetNewChunks(chunks, 1, &fromScan);
	EXPECT_TRUE(fromScan);

	hddGetNewChunks(chunks, 1, &fromScan);
	EXPECT_FALSE(fromScan);
}

/// A buffer whose one write, of @p fill bytes at @p blockNum, was replied to its client.
std::shared_ptr<InputBuffer> repliedWriteAt(uint16_t blockNum, uint8_t fill = 0) {
	auto buffer = std::make_shared<InputBuffer>(cltocs::writeData::kPrefixSize, 1);
	buffer->addNewWriteOperation();
	const std::vector<uint8_t> data(SFSBLOCKSIZE, fill);
	buffer->copyIntoBuffer(InputBuffer::BufferType::Block, data.data(), data.size());
	buffer->setupLastWriteOperation(blockNum, 0, SFSBLOCKSIZE, 1, 0);
	buffer->repliedBlocks = 1;
	return buffer;
}

TEST(AlreadyRepliedInputBufferTests, ExtentCountsOnlyWritesServedAtThePartVersion) {
	const auto type = ChunkPartType();
	constexpr uint64_t chunkId = 0x77;
	const auto ownWrite = repliedWriteAt(2);
	const auto lateWrite = repliedWriteAt(9);

	// The new owner's write at version 4, and a late session's beyond it, at version 3: the late
	// one never lengthens the part at version 4.
	hddInsertAlreadyRepliedInputBuffer(chunkId, type, 4, ownWrite, true);
	hddInsertAlreadyRepliedInputBuffer(chunkId, type, 3, lateWrite, true);
	EXPECT_EQ(hddAlreadyRepliedBlocksEnd(chunkId, type, 4), 3U);
	EXPECT_EQ(hddAlreadyRepliedBlocksEnd(chunkId, type, 3), 10U);

	// Under Master the writes carry no version, and every one of them counts.
	const auto masterWrite = repliedWriteAt(12);
	hddInsertAlreadyRepliedInputBuffer(chunkId, type, std::nullopt, masterWrite, true);
	EXPECT_EQ(hddAlreadyRepliedBlocksEnd(chunkId, type, 4), 13U);

	for (const auto &buffer : {ownWrite, lateWrite, masterWrite}) {
		hddRemoveAlreadyRepliedInputBuffer(chunkId, type, buffer);
		buffer->repliedBlocks = 0;
	}
	EXPECT_EQ(hddAlreadyRepliedBlocksEnd(chunkId, type, 4), 0U);
}

TEST(AlreadyRepliedInputBufferTests, ReadOverlayServesOnlyWritesAtThePartVersion) {
	const auto type = ChunkPartType();
	constexpr uint64_t chunkId = 0x78;
	constexpr uint16_t block = 3;
	const auto lateWrite = repliedWriteAt(block, 0xAB);
	hddInsertAlreadyRepliedInputBuffer(chunkId, type, 3, lateWrite, true);

	// The block as read from the part: zeros, with their CRC.
	const std::vector<uint8_t> zeros(SFSBLOCKSIZE, 0);
	const auto readAt = [&](uint32_t partVersion) {
		auto read = std::make_unique<OutputBuffer>(1, 1);
		uint8_t crc[kCrcSize];
		uint8_t *crcPointer = crc;
		put32bit(&crcPointer, mycrc32(0, zeros.data(), zeros.size()));
		read->copyIntoBuffer(OutputBuffer::BufferType::CRC, crc, kCrcSize);
		read->copyIntoBuffer(OutputBuffer::BufferType::Block, zeros);
		hddUpdateOutputBufferWithAlreadyRepliedInputBuffers(
		    chunkId, type, partVersion, block * SFSBLOCKSIZE, SFSBLOCKSIZE, read.get());
		return read;
	};
	const auto blockAndCrc = [](const OutputBuffer &read) {
		const uint8_t *crcPointer = read.rawData(OutputBuffer::BufferType::CRC);
		uint32_t crc = 0;
		get32bit(&crcPointer, crc);
		const uint8_t *data = read.rawData(OutputBuffer::BufferType::Block);
		return std::pair<std::vector<uint8_t>, uint32_t>({data, data + SFSBLOCKSIZE}, crc);
	};

	// A read at the late session's version gets its bytes and their CRC; a read at the part's
	// newer version gets what the part holds.
	const std::vector<uint8_t> written(SFSBLOCKSIZE, 0xAB);
	const auto [lateBlock, lateCrc] = blockAndCrc(*readAt(3));
	EXPECT_EQ(lateBlock, written);
	EXPECT_EQ(lateCrc, mycrc32(0, written.data(), written.size()));
	const auto [newBlock, newCrc] = blockAndCrc(*readAt(4));
	EXPECT_EQ(newBlock, zeros);
	EXPECT_EQ(newCrc, mycrc32(0, zeros.data(), zeros.size()));

	hddRemoveAlreadyRepliedInputBuffer(chunkId, type, lateWrite);
	lateWrite->repliedBlocks = 0;
}

/// A disk whose parts are found without their files.
class InMemoryPartsDisk : public CmrDisk {
public:
	using CmrDisk::CmrDisk;
	int updateChunkAttributes(IChunk * /*chunk*/, bool /*isFromScan*/) override {
		return SAUNAFS_STATUS_OK;
	}
};

TEST(WriteSessionOpenTests, SessionAtAnotherVersionThanThePartJoinsNoLock) {
	const disk::Configuration config("/mnt/hdd_session_open/");
	InMemoryPartsDisk disk(config);
	const auto type = ChunkPartType();
	constexpr uint64_t chunkId = 0x79;
	auto *part =
	    hddChunkFindOrCreatePlusLock(&disk, chunkId, type, disk::ChunkGetMode::kCreateOnly);
	ASSERT_NE(part, ChunkNotFound);
	part->setVersion(5);
	disk.chunks().insert(part);
	hddChunkRelease(part);

	// The part moved to version 5 before the session at version 4 opens: the open fails before
	// anything joins the lock, which the session's end would otherwise end.
	ClientJobPool pool("session-open", 1, 10, IOPriorityMode::Fifo);
	const int descriptor = pool.allocateListener(0);
	uint8_t status = SAUNAFS_STATUS_OK;
	bool joined = false;
	job_open(
	    pool, [&](uint8_t jobStatus, void *) { status = jobStatus; }, chunkId, type, 4,
	    [&] { joined = true; });
	pollfd ready{descriptor, POLLIN, 0};
	ASSERT_EQ(poll(&ready, 1, 1000), 1);
	pool.processCompletedJobs(0);
	EXPECT_EQ(status, SAUNAFS_ERROR_WRONGVERSION);
	EXPECT_FALSE(joined);

	part = hddChunkFindAndLock(chunkId, type);
	ASSERT_NE(part, ChunkNotFound);
	hddDeleteChunkFromRegistry(part);
}

}  // namespace
