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
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "chunkserver-common/chunk_trash_manager.h"
#include "chunkserver-common/cmr_chunk.h"
#include "chunkserver-common/cmr_disk.h"
#include "chunkserver-common/default_disk_manager.h"
#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/hdd_utils.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/master_connection.h"
#include "common/chunk_version_with_todel_flag.h"
#include "common/saunafs_version.h"
#include "common/slice_traits.h"
#include "protocol/matocs.h"
#include "unittests/TemporaryDirectory.h"
#include "unittests/packet.h"

// Existing disk-maintenance entry points; no test-only production API is needed.
void hddCheckDisks();
void hddReportNewChunkToMaster(uint64_t id, uint32_t version, bool todel, ChunkPartType type);

class RegistrationSweepTests : public testing::Test {
protected:
	void SetUp() override {
		gScansInProgress.store(0);
		{
			std::lock_guard chunksMapLockGuard(gChunksMapMutex);
			ASSERT_TRUE(gChunksMap.empty());
			ASSERT_TRUE(gPresentChunkTypes.empty());
			// Each case controls its own table size, including sparse-table tests.
			gChunksMap.rehash(0);
		}
		std::lock_guard masterReportsLockGuard(gMasterReportsLock);
		ASSERT_TRUE(gNewChunks.empty());
		ASSERT_TRUE(gNewChunkReportOriginRuns.empty());
	}

	void TearDown() override {
		gScansInProgress.store(0);
		hddRegistrationSweepBegin(RegistrationSweepMode::kPush);
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		while (!gChunksMap.empty()) {
			hddRemoveChunkFromContainers(gChunksMap.begin()->second.get());
		}
		EXPECT_TRUE(gPresentChunkTypes.empty());
	}

	IChunk *addChunk(uint64_t id, ChunkState state = ChunkState::Available,
	                 ChunkPartType type = ChunkPartType()) {
		auto *chunk =
		    hddChunkFindOrCreatePlusLock(&disk_, id, type, disk::ChunkGetMode::kCreateOnly);
		if (chunk == nullptr) { return nullptr; }
		{
			std::lock_guard chunksMapLockGuard(gChunksMapMutex);
			chunk->setVersion(1);
			std::lock_guard testsLockGuard(gTestsMutex);
			disk_.chunks().insert(chunk);
		}
		if (state == ChunkState::Available) { hddChunkRelease(chunk); }
		return chunk;
	}

	CmrDisk disk_{"/tmp/registration-sweep-reconciliation/",
	              "/tmp/registration-sweep-reconciliation/", false, false};
};

namespace {

class EpochCountingChunk : public CmrChunk {
public:
	using CmrChunk::CmrChunk;

	uint32_t registrationEpoch() const override {
		++epochReads_;
		return CmrChunk::registrationEpoch();
	}

	void resetEpochReads() { epochReads_ = 0; }
	uint64_t epochReads() const { return epochReads_; }

private:
	mutable uint64_t epochReads_{0};
};

class EpochCountingDisk : public CmrDisk {
public:
	using CmrDisk::CmrDisk;

	IChunk *instantiateNewConcreteChunk(uint64_t id, ChunkPartType type) override {
		auto *chunk = new EpochCountingChunk(id, type, ChunkState::Locked);
		chunk->setOwner(this);
		return chunk;
	}

	// Reload registry state without requiring real files or a mounted disk.
	void refreshDataDiskUsage() override {}
};

}  // namespace

class RegistrationDiskReloadTests : public RegistrationSweepTests {
protected:
	void SetUp() override {
		RegistrationSweepTests::SetUp();
		if (HasFatalFailure()) { return; }
		ASSERT_TRUE(gDisks.empty());
		savedDiskManager_ = std::move(gDiskManager);
		gDiskManager = std::make_unique<DefaultDiskManager>();
		savedDiskActions_ = std::exchange(gDiskActions, 1);
		savedResetTester_ = gResetTester.load();
		savedSpaceChanged_ = gHddSpaceChanged.load();
	}

	void TearDown() override {
		// Join outside gDisksMutex: the scanner takes it before finishing.
		for (auto &disk : gDisks) {
			if (disk->scanThread().joinable()) { disk->scanThread().join(); }
			ChunkTrashManager::eraseDisk(disk->metaPath());
		}
		RegistrationSweepTests::TearDown();
		gDisks.clear();
		gDiskManager = std::move(savedDiskManager_);
		gDiskActions = savedDiskActions_;
		gResetTester.store(savedResetTester_);
		gHddSpaceChanged.store(savedSpaceChanged_);
	}

	DefaultDiskManager &manager() { return static_cast<DefaultDiskManager &>(*gDiskManager); }

	TemporaryDirectory directory_{"/tmp", "registration-disk-reload"};
	std::unique_ptr<IDiskManager> savedDiskManager_;
	uint8_t savedDiskActions_{0};
	bool savedResetTester_{false};
	bool savedSpaceChanged_{false};
};

TEST_F(RegistrationDiskReloadTests, FlagReloadPreservesPendingScanAndAllowsSweepToFinish) {
	const auto path = directory_.name() + "/";
	ASSERT_EQ(2, manager().parseCfgLine(path));
	ASSERT_EQ(1, gScansInProgress.load());

	// Reload before hddCheckDisks starts the counted scan, in both flag directions.
	ASSERT_EQ(1, manager().parseCfgLine("*" + path));
	EXPECT_TRUE(gDisks.front()->isMarkedForRemoval());
	EXPECT_EQ(IDisk::ScanState::kNeeded, gDisks.front()->scanState());
	ASSERT_EQ(1, manager().parseCfgLine(path));
	EXPECT_EQ(IDisk::ScanState::kNeeded, gDisks.front()->scanState());
	ASSERT_EQ(1, gScansInProgress.load());

	std::vector<ChunkWithVersionAndType> bulk;
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 10));

	hddCheckDisks();
	EXPECT_TRUE(gDisks.front()->scanThread().joinable());
	if (gDisks.front()->scanThread().joinable()) { gDisks.front()->scanThread().join(); }
	EXPECT_EQ(IDisk::ScanState::kThreadFinished, gDisks.front()->scanState());
	EXPECT_EQ(0, gScansInProgress.load());
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 10));
}

TEST_F(RegistrationDiskReloadTests, FlagReloadStillReannouncesAnAlreadyScannedDisk) {
	const auto path = directory_.name() + "/";
	ASSERT_EQ(2, manager().parseCfgLine(path));
	hddCheckDisks();
	ASSERT_TRUE(gDisks.front()->scanThread().joinable());
	gDisks.front()->scanThread().join();
	gDisks.front()->setScanState(IDisk::ScanState::kWorking);

	ASSERT_EQ(1, manager().parseCfgLine("*" + path));
	EXPECT_EQ(IDisk::ScanState::kSendNeeded, gDisks.front()->scanState());
	hddCheckDisks();
	EXPECT_EQ(IDisk::ScanState::kWorking, gDisks.front()->scanState());
	EXPECT_EQ(0, gScansInProgress.load());
}

class RegistrationSweepTrackingTests : public RegistrationSweepTests {
protected:
	void SetUp() override {
		RegistrationSweepTests::SetUp();
		if (HasFatalFailure()) { return; }
		std::lock_guard disksLockGuard(gDisksMutex);
		ASSERT_TRUE(gDisks.empty());
		auto disk = std::make_unique<EpochCountingDisk>(
		    "/tmp/registration-sweep-tracking/", "/tmp/registration-sweep-tracking/", false, false);
		countingDisk_ = disk.get();
		countingDisk_->setScanState(IDisk::ScanState::kWorking);
		gDisks.push_back(std::move(disk));
		savedDiskManager_ = std::move(gDiskManager);
		gDiskManager = std::make_unique<DefaultDiskManager>();
		savedDiskActions_ = std::exchange(gDiskActions, 1);
		savedSpaceChanged_ = gHddSpaceChanged.load();
	}

	void TearDown() override {
		if (countingDisk_ == nullptr) { return; }
		gScansInProgress.store(0);
		hddRegistrationSweepBegin(RegistrationSweepMode::kPush);
		{
			std::lock_guard chunksMapLockGuard(gChunksMapMutex);
			while (!gChunksMap.empty()) {
				hddRemoveChunkFromContainers(gChunksMap.begin()->second.get());
			}
			EXPECT_TRUE(gPresentChunkTypes.empty());
		}
		std::lock_guard disksLockGuard(gDisksMutex);
		gDisks.clear();
		gDiskManager = std::move(savedDiskManager_);
		gDiskActions = savedDiskActions_;
		gHddSpaceChanged.store(savedSpaceChanged_);
	}

	EpochCountingChunk *addTrackedChunk(uint64_t id, ChunkState state = ChunkState::Available,
	                                    ChunkPartType type = ChunkPartType(),
	                                    uint32_t version = 1) {
		// Use the production insertion path so future sweep notifications are exercised.
		auto *chunk =
		    hddChunkFindOrCreatePlusLock(countingDisk_, id, type, disk::ChunkGetMode::kCreateOnly);
		if (chunk == nullptr) { return nullptr; }
		{
			std::lock_guard chunksMapLockGuard(gChunksMapMutex);
			chunk->setVersion(version);
			std::lock_guard testsLockGuard(gTestsMutex);
			countingDisk_->chunks().insert(chunk);
		}
		if (state == ChunkState::Available) { hddChunkRelease(chunk); }
		return static_cast<EpochCountingChunk *>(chunk);
	}

	void reannounceDisk(bool markedForDeletion) {
		{
			std::lock_guard disksLockGuard(gDisksMutex);
			countingDisk_->setIsMarkedForRemoval(markedForDeletion);
			countingDisk_->setScanState(IDisk::ScanState::kSendNeeded);
		}
		hddCheckDisks();
	}

	EpochCountingDisk *countingDisk_{nullptr};
	std::unique_ptr<IDiskManager> savedDiskManager_;
	uint8_t savedDiskActions_{0};
	bool savedSpaceChanged_{false};
};

TEST_F(RegistrationSweepTrackingTests, LockedRetriesDoNotRevisitReportedChunks) {
	constexpr uint64_t kReportedChunks = 4096;
	std::vector<EpochCountingChunk *> reported;
	for (uint64_t id = 1; id <= kReportedChunks; ++id) {
		auto *chunk = addTrackedChunk(id);
		ASSERT_NE(chunk, nullptr);
		reported.push_back(chunk);
	}
	auto *locked = addTrackedChunk(kReportedChunks + 1, ChunkState::Locked);
	ASSERT_NE(locked, nullptr);

	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(RegistrationSweepResult::kBulkReady,
	          hddRegistrationSweepNext(bulk, kReportedChunks + 1));
	ASSERT_EQ(kReportedChunks, bulk.size());
	ASSERT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, kReportedChunks + 1));
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		for (auto *chunk : reported) { chunk->resetEpochReads(); }
	}
	for (unsigned retry = 0; retry < 3; ++retry) {
		EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
		EXPECT_TRUE(bulk.empty());
	}
	{
		// Count actual registry visits, not elapsed time or mutex acquisition latency.
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		uint64_t reads = 0;
		for (auto *chunk : reported) { reads += chunk->epochReads(); }
		EXPECT_EQ(0U, reads) << "Locked retries must only revisit outstanding entries";
	}
	hddChunkRelease(locked);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(kReportedChunks + 1, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, RehashRestartHasBoundedVisitWork) {
	constexpr std::size_t kReportedChunks = 4 * kDefaultChunkBulkSize;
	std::vector<EpochCountingChunk *> reported;
	for (uint64_t id = 1; id <= kReportedChunks; ++id) {
		auto *chunk = addTrackedChunk(id);
		ASSERT_NE(chunk, nullptr);
		reported.push_back(chunk);
	}
	auto *locked = addTrackedChunk(kReportedChunks + 1, ChunkState::Locked);
	ASSERT_NE(locked, nullptr);

	// Report the available inventory, leaving the walk unfinished at its last bulk.
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, kReportedChunks));
	ASSERT_EQ(kReportedChunks, bulk.size());
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		ASSERT_FALSE(gRegistrationSweepState.cursorDone);
		gChunksMap.rehash(gChunksMap.bucket_count() * 2);
		for (auto *chunk : reported) { chunk->resetEpochReads(); }
	}

	// Count actual revisits: no large inventory or timing threshold is needed.
	EXPECT_EQ(RegistrationSweepResult::kYield, hddRegistrationSweepNext(bulk, 1));
	EXPECT_TRUE(bulk.empty());
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		uint64_t reads = 0;
		for (auto *chunk : reported) { reads += chunk->epochReads(); }
		EXPECT_GT(reads, 0U);
		EXPECT_LE(reads, kDefaultChunkBulkSize);
	}

	// Bounded retries must still reach the end without reporting old entries again.
	for (std::size_t call = 0; call < 4 * kReportedChunks; ++call) {
		const auto result = hddRegistrationSweepNext(bulk, 1);
		EXPECT_TRUE(result == RegistrationSweepResult::kYield ||
		            result == RegistrationSweepResult::kRetry);
		EXPECT_TRUE(bulk.empty());
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		if (gRegistrationSweepState.cursorDone) { break; }
	}
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		ASSERT_TRUE(gRegistrationSweepState.cursorDone);
	}
	hddChunkRelease(locked);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(kReportedChunks + 1, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, SparseRegistryYieldsWhileSkippingEmptyBuckets) {
	std::size_t bucketCount;
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		gChunksMap.rehash(4 * kDefaultChunkBulkSize);
		bucketCount = gChunksMap.bucket_count();
	}
	// A high bucket forces work even though the registry contains only one chunk.
	ASSERT_NE(addTrackedChunk(bucketCount - 1), nullptr);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	EXPECT_EQ(RegistrationSweepResult::kYield, hddRegistrationSweepNext(bulk, 1));
	EXPECT_TRUE(bulk.empty());
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		EXPECT_GT(gRegistrationSweepState.bucket, 0U);
		EXPECT_LE(gRegistrationSweepState.bucket, kDefaultChunkBulkSize);
	}

	for (std::size_t call = 0; call < bucketCount; ++call) {
		if (hddRegistrationSweepNext(bulk, 1) == RegistrationSweepResult::kBulkReady) { break; }
	}
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(bucketCount - 1, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, CollisionBucketResumesAfterYield) {
	constexpr std::size_t kChunks = 2 * kDefaultChunkBulkSize + 1;
	std::size_t bucketCount;
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		gChunksMap.reserve(kChunks);
		bucketCount = gChunksMap.bucket_count();
	}
	std::vector<EpochCountingChunk *> chunks;
	for (std::size_t index = 0; index < kChunks; ++index) {
		// Hashing uses the ID, so these distinct keys all land in bucket 1.
		auto *chunk = addTrackedChunk(1 + index * bucketCount, ChunkState::Locked);
		ASSERT_NE(chunk, nullptr);
		chunks.push_back(chunk);
	}
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	EXPECT_EQ(RegistrationSweepResult::kYield, hddRegistrationSweepNext(bulk, 1));
	EXPECT_TRUE(bulk.empty());
	uint64_t reads = 0;
	for (auto *chunk : chunks) { reads += chunk->epochReads(); }
	EXPECT_LE(reads, kDefaultChunkBulkSize);

	// Resume inside the same bucket, then recover its earlier locked entries.
	for (auto *chunk : chunks) { hddChunkRelease(chunk); }
	std::set<uint64_t> reported;
	for (std::size_t call = 0; call < 4 * kChunks; ++call) {
		for (auto *chunk : chunks) { chunk->resetEpochReads(); }
		const auto result = hddRegistrationSweepNext(bulk, 7);
		reads = 0;
		for (auto *chunk : chunks) { reads += chunk->epochReads(); }
		EXPECT_LE(reads, kDefaultChunkBulkSize);
		for (const auto &chunk : bulk) { EXPECT_TRUE(reported.insert(chunk.id).second); }
		if (result == RegistrationSweepResult::kComplete) { break; }
		EXPECT_TRUE(result == RegistrationSweepResult::kYield ||
		            result == RegistrationSweepResult::kBulkReady);
	}
	EXPECT_EQ(kChunks, reported.size());
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, RemovalAtSavedPositionKeepsCursorValid) {
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		gChunksMap.reserve(3);
	}
	const auto bucketCount = hddGetChunkRegistryBucketCount();
	std::set<uint64_t> expected{1, 1 + bucketCount, 1 + 2 * bucketCount};
	for (const auto id : expected) { ASSERT_NE(addTrackedChunk(id), nullptr); }
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	std::set<uint64_t> reported{bulk.front().id};
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		auto &sweep = gRegistrationSweepState;
		ASSERT_TRUE(sweep.bucketPosition.has_value());
		ASSERT_NE(*sweep.bucketPosition, gChunksMap.end(sweep.bucket));
		auto *removed = (*sweep.bucketPosition)->second.get();
		expected.erase(removed->id());
		hddRemoveChunkFromContainers(removed);
	}

	for (unsigned call = 0; call < 4; ++call) {
		const auto result = hddRegistrationSweepNext(bulk, 1);
		for (const auto &chunk : bulk) { EXPECT_TRUE(reported.insert(chunk.id).second); }
		if (result == RegistrationSweepResult::kComplete) { break; }
		EXPECT_EQ(RegistrationSweepResult::kBulkReady, result);
	}
	EXPECT_EQ(expected, reported);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, InsertionIntoCurrentBucketIsNotLost) {
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		gChunksMap.reserve(3);
	}
	const auto bucketCount = hddGetChunkRegistryBucketCount();
	ASSERT_NE(addTrackedChunk(1), nullptr);
	ASSERT_NE(addTrackedChunk(1 + bucketCount), nullptr);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	std::set<uint64_t> reported{bulk.front().id};

	// Inserting at the head of this bucket must not bypass the saved cursor.
	ASSERT_NE(addTrackedChunk(1 + 2 * bucketCount), nullptr);
	ASSERT_EQ(bucketCount, hddGetChunkRegistryBucketCount());
	for (unsigned call = 0; call < 4; ++call) {
		const auto result = hddRegistrationSweepNext(bulk, 1);
		for (const auto &chunk : bulk) { EXPECT_TRUE(reported.insert(chunk.id).second); }
		if (result == RegistrationSweepResult::kComplete) { break; }
		EXPECT_EQ(RegistrationSweepResult::kBulkReady, result);
	}
	const std::set<uint64_t> expected{1, 1 + bucketCount, 1 + 2 * bucketCount};
	EXPECT_EQ(expected, reported);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, PicksUpInsertionsAfterCursorWithoutRehash) {
	auto *locked = addTrackedChunk(1, ChunkState::Locked);
	ASSERT_NE(locked, nullptr);
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		gChunksMap.reserve(16);
	}
	const auto buckets = hddGetChunkRegistryBucketCount();
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));

	ASSERT_NE(addTrackedChunk(2), nullptr);
	ASSERT_EQ(buckets, hddGetChunkRegistryBucketCount());
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(2U, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	hddChunkRelease(locked);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(1U, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, PicksUpInsertionsBehindUnfinishedCursor) {
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		gChunksMap.reserve(128);
		ASSERT_LT(gChunksMap.bucket(makeChunkKey(2, ChunkPartType())),
		          gChunksMap.bucket(makeChunkKey(32, ChunkPartType())));
	}
	ASSERT_NE(addTrackedChunk(1), nullptr);
	ASSERT_NE(addTrackedChunk(32), nullptr);
	auto *locked = addTrackedChunk(63, ChunkState::Locked);
	ASSERT_NE(locked, nullptr);
	const auto buckets = hddGetChunkRegistryBucketCount();
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	for (uint64_t id : {1, 32}) {
		ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
		ASSERT_EQ(1U, bulk.size());
		ASSERT_EQ(id, bulk.front().id);
	}

	ASSERT_NE(addTrackedChunk(2), nullptr);
	ASSERT_EQ(buckets, hddGetChunkRegistryBucketCount());
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(2U, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	hddChunkRelease(locked);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(63U, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, HandlesTwoRehashesBetweenCredits) {
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		gChunksMap.reserve(128);
	}
	ASSERT_NE(addTrackedChunk(1), nullptr);
	ASSERT_NE(addTrackedChunk(32), nullptr);
	auto *locked = addTrackedChunk(63, ChunkState::Locked);
	ASSERT_NE(locked, nullptr);
	const auto buckets = hddGetChunkRegistryBucketCount();
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	for (uint64_t id : {1, 32}) {
		ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
		ASSERT_EQ(1U, bulk.size());
		ASSERT_EQ(id, bulk.front().id);
	}
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		gChunksMap.rehash(buckets * 2);
		ASSERT_GT(gChunksMap.bucket_count(), buckets);
	}
	ASSERT_NE(addTrackedChunk(2), nullptr);
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		gChunksMap.rehash(buckets);
		ASSERT_EQ(buckets, gChunksMap.bucket_count());
	}
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(2U, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	hddChunkRelease(locked);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(63U, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, LockedBacklogHasBoundedRetryWork) {
	constexpr std::size_t kLockedChunks = 2 * kDefaultChunkBulkSize + 1;
	std::vector<EpochCountingChunk *> locked;
	for (std::size_t id = 1; id <= kLockedChunks; ++id) {
		auto *chunk = addTrackedChunk(id, ChunkState::Locked);
		ASSERT_NE(chunk, nullptr);
		locked.push_back(chunk);
	}
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	for (std::size_t call = 0; call < kLockedChunks; ++call) {
		if (hddRegistrationSweepNext(bulk, 1) != RegistrationSweepResult::kYield) { break; }
		ASSERT_TRUE(bulk.empty());
	}
	ASSERT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		for (auto *chunk : locked) { chunk->resetEpochReads(); }
	}
	EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	EXPECT_TRUE(bulk.empty());
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		uint64_t reads = 0;
		for (auto *chunk : locked) {
			reads += chunk->epochReads();
			EXPECT_LE(chunk->epochReads(), 1U);
		}
		EXPECT_GT(reads, 0U);
		EXPECT_LE(reads, kDefaultChunkBulkSize);
	}
	for (auto *chunk : locked) { hddChunkRelease(chunk); }
	std::set<uint64_t> reported;
	for (std::size_t calls = 0; calls < kLockedChunks; ++calls) {
		const auto result = hddRegistrationSweepNext(bulk, 7);
		if (result == RegistrationSweepResult::kComplete) { break; }
		ASSERT_EQ(RegistrationSweepResult::kBulkReady, result);
		ASSERT_LE(bulk.size(), 7U);
		for (const auto &chunk : bulk) { EXPECT_TRUE(reported.insert(chunk.id).second); }
	}
	EXPECT_EQ(kLockedChunks, reported.size());
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, DropsRemovedPendingKeysAndReportsReplacementVersion) {
	auto *removed = addTrackedChunk(1, ChunkState::Locked);
	auto *anchor = addTrackedChunk(2, ChunkState::Locked);
	ASSERT_NE(removed, nullptr);
	ASSERT_NE(anchor, nullptr);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));

	hddDeleteChunkFromRegistry(removed);
	EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	EXPECT_TRUE(bulk.empty());
	ASSERT_NE(addTrackedChunk(1, ChunkState::Available, ChunkPartType(), 7), nullptr);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(1U, bulk.front().id);
	EXPECT_EQ(7U, bulk.front().version);
	EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	hddDeleteChunkFromRegistry(anchor);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
	EXPECT_TRUE(bulk.empty());
}

TEST_F(RegistrationSweepTrackingTests, RehashPreservesPendingKeysWithoutDuplicateReports) {
	auto *first = addTrackedChunk(1, ChunkState::Locked);
	auto *second = addTrackedChunk(2, ChunkState::Locked);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);
	ASSERT_NE(addTrackedChunk(3), nullptr);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 3));
	ASSERT_EQ(1U, bulk.size());
	ASSERT_EQ(3U, bulk.front().id);
	ASSERT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		const auto buckets = gChunksMap.bucket_count();
		gChunksMap.rehash(buckets * 2);
		ASSERT_GT(gChunksMap.bucket_count(), buckets);
	}
	hddChunkRelease(second);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(2U, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	hddChunkRelease(first);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(1U, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, ReannouncesSweptDiskStateWhileLockedKeysRemain) {
	ASSERT_NE(addTrackedChunk(1), nullptr);
	auto *locked = addTrackedChunk(2, ChunkState::Locked);
	ASSERT_NE(locked, nullptr);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 2));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(1U, bulk.front().version);
	ASSERT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 2));

	reannounceDisk(true);
	reannounceDisk(true);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(1U, bulk.front().id);
	EXPECT_EQ(common::combineVersionWithTodelFlag(1, true), bulk.front().version);
	EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	reannounceDisk(false);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(1U, bulk.front().id);
	EXPECT_EQ(1U, bulk.front().version);
	EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	hddChunkRelease(locked);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(2U, bulk.front().id);
	EXPECT_EQ(1U, bulk.front().version);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, RetainsDiskReannouncementAfterSweepCompletes) {
	ASSERT_NE(addTrackedChunk(1), nullptr);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));

	reannounceDisk(true);
	hddGetNewChunks(bulk, 1);
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(1U, bulk.front().id);
	EXPECT_EQ(common::combineVersionWithTodelFlag(1, true), bulk.front().version);
	hddGetNewChunks(bulk, 1);
	EXPECT_TRUE(bulk.empty());
}

TEST_F(RegistrationSweepTrackingTests, PendingScansDoNotRevisitReportedChunksAndCaptureArrivals) {
	constexpr uint64_t kReportedChunks = 128;
	std::vector<EpochCountingChunk *> reported;
	for (uint64_t id = 1; id <= kReportedChunks; ++id) {
		auto *chunk = addTrackedChunk(id);
		ASSERT_NE(chunk, nullptr);
		reported.push_back(chunk);
	}
	gScansInProgress.store(1);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(RegistrationSweepResult::kBulkReady,
	          hddRegistrationSweepNext(bulk, kReportedChunks + 1));
	ASSERT_EQ(kReportedChunks, bulk.size());
	ASSERT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		for (auto *chunk : reported) { chunk->resetEpochReads(); }
	}
	for (unsigned retry = 0; retry < 3; ++retry) {
		EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
		EXPECT_TRUE(bulk.empty());
	}
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		uint64_t reads = 0;
		for (auto *chunk : reported) { reads += chunk->epochReads(); }
		EXPECT_EQ(0U, reads) << "Waiting for scans must not rescan reported entries";
	}
	ASSERT_NE(addTrackedChunk(kReportedChunks + 1), nullptr);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(kReportedChunks + 1, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	gScansInProgress.store(0);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, PendingEcPartsRespectBulkLimitAndDoNotStarve) {
	const auto lockedType = slice_traits::ec::ChunkPartType(2, 1, 0);
	const auto firstType = slice_traits::ec::ChunkPartType(2, 1, 1);
	const auto secondType = slice_traits::ec::ChunkPartType(2, 1, 2);
	auto *locked = addTrackedChunk(1, ChunkState::Locked, lockedType);
	auto *first = addTrackedChunk(1, ChunkState::Locked, firstType);
	auto *second = addTrackedChunk(1, ChunkState::Locked, secondType);
	ASSERT_NE(locked, nullptr);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	hddChunkRelease(first);
	hddChunkRelease(second);
	std::set<ChunkPartType> reportedTypes;
	for (unsigned part = 0; part < 2; ++part) {
		ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
		ASSERT_EQ(1U, bulk.size());
		EXPECT_EQ(1U, bulk.front().id);
		EXPECT_NE(lockedType, bulk.front().type);
		EXPECT_TRUE(reportedTypes.insert(bulk.front().type).second);
	}
	EXPECT_EQ((std::set<ChunkPartType>{firstType, secondType}), reportedTypes);
	EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	hddChunkRelease(locked);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(lockedType, bulk.front().type);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, NewSessionDiscardsRemovedPendingKeys) {
	auto *removed = addTrackedChunk(1, ChunkState::Locked);
	ASSERT_NE(removed, nullptr);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	hddDeleteChunkFromRegistry(removed);

	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
	ASSERT_NE(addTrackedChunk(2), nullptr);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(2U, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTrackingTests, PushFallbackDiscardsPendingKeysAndAllowsAnnouncements) {
	auto *removed = addTrackedChunk(1, ChunkState::Locked);
	ASSERT_NE(removed, nullptr);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	ASSERT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	hddDeleteChunkFromRegistry(removed);

	hddRegistrationSweepBegin(RegistrationSweepMode::kPush);
	ASSERT_NE(addTrackedChunk(2), nullptr);
	hddReportNewChunkToMaster(2, 1, false, ChunkPartType());
	hddGetNewChunks(bulk, 1);
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(2U, bulk.front().id);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	ASSERT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(2U, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTests, LockedChunkIsRetriedWithoutWaiting) {
	auto *chunk = addChunk(1, ChunkState::Locked);
	ASSERT_NE(chunk, nullptr);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
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
		hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
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
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
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
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
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
	ASSERT_NE(addChunk(1), nullptr);
	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);

	std::vector<ChunkWithVersionAndType> swept;
	ASSERT_EQ(hddRegistrationSweepNext(swept, 1), RegistrationSweepResult::kBulkReady);
	ASSERT_EQ(swept.size(), 1U);
	ASSERT_EQ(swept.front().id, 1U);

	ASSERT_NE(addChunk(3), nullptr);

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

TEST_F(RegistrationSweepTrackingTests, YieldRetainsCreditWithoutLockedRetryDelay) {
	constexpr std::size_t kChunks = kDefaultChunkBulkSize + 1;
	std::vector<EpochCountingChunk *> chunks;
	for (uint64_t id = 1; id <= kChunks; ++id) {
		auto *chunk = addTrackedChunk(id, ChunkState::Locked);
		ASSERT_NE(chunk, nullptr);
		chunks.push_back(chunk);
	}
	MasterConn connection("localhost", "9420", "default", nullptr, nullptr);
	connection.setMode(ConnectionMode::CONNECTED);
	auto registered = matocs::registerHost::build(SAUNAFS_STATUS_OK, SAUNAFS_VERSHEX, "default");
	removeHeaderInPlace(registered);
	connection.onRegistered(registered);
	auto start = matocs::registerChunksStart::build(1, 1);
	removeHeaderInPlace(start);
	connection.onRegisterChunksStart(start);
	ASSERT_EQ(RegistrationStatus::kChunksRegistering, connection.registrationStatus());

	// START exhausted the cursor budget, retaining its only credit. Make the next
	// entry available: the next turn must send it without another CREDIT or sleep.
	hddChunkRelease(chunks.back());
	connection.checkPullRegistration();
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		EXPECT_NE(0U, chunks.back()->registrationEpoch());
		for (std::size_t index = 0; index + 1 < chunks.size(); ++index) {
			EXPECT_EQ(0U, chunks[index]->registrationEpoch());
		}
	}

	// Normal pacing resumes once that retained credit has been consumed.
	for (std::size_t index = 0; index + 1 < chunks.size(); ++index) {
		hddChunkRelease(chunks[index]);
	}
	auto credit = matocs::registerChunksCredit::build(kChunks + 1);
	removeHeaderInPlace(credit);
	connection.onRegisterChunksCredit(credit);
	EXPECT_EQ(RegistrationStatus::kChunksRegistered, connection.registrationStatus());
}

TEST_F(RegistrationSweepTests, StartsWithoutPreRegistrationNewChunkQueue) {
	ASSERT_NE(addChunk(1), nullptr);
	const auto type = ChunkPartType();
	hddEnqueueChunkReport(ChunkWithVersionAndType(1, 1, type), true);

	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);

	{
		std::lock_guard masterReportsLockGuard(gMasterReportsLock);
		EXPECT_TRUE(gNewChunkReportOriginRuns.empty());
	}

	std::vector<ChunkWithVersionAndType> newChunks;
	hddGetNewChunks(newChunks, 1);
	EXPECT_TRUE(newChunks.empty());

	std::vector<ChunkWithVersionAndType> bulk;
	EXPECT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(1U, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}

TEST_F(RegistrationSweepTests, WaitsForPendingDiskScansBeforeCompleting) {
	gScansInProgress.store(1);

	hddRegistrationSweepBegin(RegistrationSweepMode::kPull);
	std::vector<ChunkWithVersionAndType> bulk;
	EXPECT_EQ(RegistrationSweepResult::kRetry, hddRegistrationSweepNext(bulk, 1));
	EXPECT_TRUE(bulk.empty());

	ASSERT_NE(addChunk(1), nullptr);

	gScansInProgress.store(0);
	EXPECT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(1U, bulk.size());
	EXPECT_EQ(1U, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}
