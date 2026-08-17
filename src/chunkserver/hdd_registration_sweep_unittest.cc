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

#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "chunkserver-common/cmr_chunk.h"
#include "chunkserver-common/cmr_disk.h"
#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/hdd_utils.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/master_connection.h"
#include "common/saunafs_version.h"
#include "common/slice_traits.h"
#include "protocol/matocs.h"
#include "unittests/packet.h"

class RegistrationSweepTests : public testing::Test {
protected:
	void SetUp() override {
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

	IChunk *addChunk(uint64_t id, ChunkState state = ChunkState::Available,
	                 ChunkPartType type = ChunkPartType()) {
		auto chunk = std::make_unique<CmrChunk>(id, type, state);
		chunk->setOwner(&disk_);
		chunk->setVersion(1);
		auto *chunkPtr = chunk.get();
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		if (!gChunksMap.emplace(makeChunkKey(id, type), std::move(chunk)).second) {
			return nullptr;
		}
		hddNotePresentChunkType(type);
		return chunkPtr;
	}

	CmrDisk disk_{"/tmp/registration-sweep-reconciliation/",
	              "/tmp/registration-sweep-reconciliation/", false, false};
};

TEST_F(RegistrationSweepTests, LockedChunkIsRetriedWithoutWaiting) {
	auto *chunk = addChunk(1, ChunkState::Locked);
	ASSERT_NE(chunk, nullptr);
	hddRegistrationSweepBegin();
	std::vector<ChunkWithVersionAndType> bulk;
	auto sweep = std::async(std::launch::async,
	                        [&bulk] { return hddRegistrationSweepNext(bulk, 1); });
	EXPECT_EQ(sweep.wait_for(std::chrono::seconds(1)), std::future_status::ready);
	// Release before joining so a blocking implementation fails without hanging.
	hddChunkRelease(chunk);
	EXPECT_EQ(sweep.get(), RegistrationSweepResult::kRetry);
	EXPECT_TRUE(bulk.empty());
	EXPECT_EQ(hddRegistrationSweepNext(bulk, 1), RegistrationSweepResult::kBulkReady);
	ASSERT_EQ(bulk.size(), 1U);
	EXPECT_EQ(bulk.front().id, 1U);
	EXPECT_EQ(hddRegistrationSweepNext(bulk, 1), RegistrationSweepResult::kComplete);
}

TEST_F(RegistrationSweepTests, EpochSurvivesMoreThan255Sessions) {
	auto *chunk = addChunk(1);
	ASSERT_NE(chunk, nullptr);
	std::vector<ChunkWithVersionAndType> bulk;
	for (unsigned session = 0; session < 300; ++session) {
		hddRegistrationSweepBegin();
		ASSERT_EQ(hddRegistrationSweepNext(bulk, 1), RegistrationSweepResult::kBulkReady);
		ASSERT_EQ(bulk.size(), 1U);
		ASSERT_EQ(hddRegistrationSweepNext(bulk, 1), RegistrationSweepResult::kComplete);
	}
	std::lock_guard chunksMapLockGuard(gChunksMapMutex);
	EXPECT_GT(chunk->registrationEpoch(), 255U);
}

TEST_F(RegistrationSweepTests, PartsInOneBucketRespectBulkLimit) {
	ASSERT_NE(addChunk(1), nullptr);
	ASSERT_NE(addChunk(1, ChunkState::Available, slice_traits::xors::ChunkPartType(2, 1)),
	          nullptr);
	ASSERT_NE(addChunk(1, ChunkState::Available, slice_traits::xors::ChunkPartType(2, 2)),
	          nullptr);
	hddRegistrationSweepBegin();
	std::vector<ChunkWithVersionAndType> bulk;
	for (unsigned part = 0; part < 3; ++part) {
		ASSERT_EQ(hddRegistrationSweepNext(bulk, 1), RegistrationSweepResult::kBulkReady);
		EXPECT_EQ(bulk.size(), 1U);
	}
	EXPECT_EQ(hddRegistrationSweepNext(bulk, 1), RegistrationSweepResult::kComplete);
}

TEST_F(RegistrationSweepTests, RehashDoesNotLoseUnreportedChunks) {
	ASSERT_NE(addChunk(1), nullptr);
	ASSERT_NE(addChunk(2), nullptr);
	hddRegistrationSweepBegin();
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(hddRegistrationSweepNext(bulk, 1), RegistrationSweepResult::kBulkReady);
	const auto firstId = bulk.front().id;
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		gChunksMap.rehash(gChunksMap.bucket_count() * 2);
	}
	ASSERT_EQ(hddRegistrationSweepNext(bulk, 1), RegistrationSweepResult::kBulkReady);
	ASSERT_EQ(bulk.size(), 1U);
	EXPECT_NE(bulk.front().id, firstId);
	EXPECT_EQ(hddRegistrationSweepNext(bulk, 1), RegistrationSweepResult::kComplete);
}

TEST_F(RegistrationSweepTests, DropsReportedAndRemovedAnnouncements) {
	auto *reported = addChunk(1);
	ASSERT_NE(reported, nullptr);
	ASSERT_NE(addChunk(3), nullptr);
	hddRegistrationSweepBegin();
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		hddRegistrationSweepMarkRegistered(*reported);
	}
	const auto type = ChunkPartType();
	hddEnqueueChunkReport(ChunkWithVersionAndType(1, 1, type), true);
	hddEnqueueChunkReport(ChunkWithVersionAndType(2, 1, type), true);
	hddEnqueueChunkReport(ChunkWithVersionAndType(3, 1, type), false);
	std::vector<ChunkWithVersionAndType> bulk;
	bool fromScan = false;
	hddGetNewChunks(bulk, 3, &fromScan);
	ASSERT_EQ(bulk.size(), 1U);
	EXPECT_EQ(bulk.front().id, 3U);
	EXPECT_TRUE(fromScan);
	EXPECT_TRUE(gNewChunkReportOriginRuns.empty());
}

TEST_F(RegistrationSweepTests, ConnectionRetriesWithRetainedCredit) {
	auto *chunk = addChunk(1, ChunkState::Locked);
	ASSERT_NE(chunk, nullptr);
	MasterConn connection("localhost", "9420", "default", nullptr, nullptr);
	connection.setMode(ConnectionMode::CONNECTED);
	auto registered = matocs::registerHost::build(SAUNAFS_STATUS_OK, SAUNAFS_VERSHEX,
	                                             "default");
	removeHeaderInPlace(registered);
	connection.onRegistered(registered);
	ASSERT_EQ(connection.registrationStatus(), RegistrationStatus::kAwaitingPullStart);
	auto start = matocs::registerChunksStart::build(1, 1);
	removeHeaderInPlace(start);
	connection.onRegisterChunksStart(start);
	EXPECT_EQ(connection.registrationStatus(), RegistrationStatus::kChunksRegistering);
	hddChunkRelease(chunk);
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	connection.checkPullRegistration();
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		EXPECT_NE(chunk->registrationEpoch(), 0U);
	}
	EXPECT_EQ(connection.registrationStatus(), RegistrationStatus::kChunksRegistering);
	auto credit = matocs::registerChunksCredit::build(1);
	removeHeaderInPlace(credit);
	connection.onRegisterChunksCredit(credit);
	EXPECT_EQ(connection.registrationStatus(), RegistrationStatus::kChunksRegistered);
}
