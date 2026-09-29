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

#include <vector>

#include "chunkserver-common/hdd_utils.h"
#include "chunkserver/hddspacemgr.h"

namespace {

class NewChunkReportQueueTests : public testing::Test {
protected:
	void SetUp() override { hddDiscardNewChunks(); }
	void TearDown() override { hddDiscardNewChunks(); }

	void enqueue(uint64_t id, bool fromDiskScan) {
		const auto type = ChunkPartType();
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

}  // namespace
