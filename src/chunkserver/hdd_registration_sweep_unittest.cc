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

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

#include <gtest/gtest.h>

#include "chunkserver-common/cmr_chunk.h"
#include "chunkserver-common/cmr_disk.h"
#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/hdd_utils.h"
#include "chunkserver/hddspacemgr.h"
#include "common/slice_traits.h"

class RegistrationSweepTests : public testing::Test {
protected:
	void SetUp() override {
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		ASSERT_TRUE(gChunksMap.empty());
		ASSERT_TRUE(gPresentChunkTypes.empty());
	}

	void TearDown() override {
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		gChunksMap.clear();
		gPresentChunkTypes.clear();
	}

	IChunk *addChunk(ChunkState state) {
		const auto type = slice_traits::standard::ChunkPartType();
		auto chunk = std::make_unique<CmrChunk>(1, type, state);
		chunk->setOwner(&disk_);
		auto *chunkPtr = chunk.get();

		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		const bool inserted = gChunksMap.emplace(makeChunkKey(1, type), std::move(chunk)).second;
		if (!inserted) { return nullptr; }
		hddNotePresentChunkType(type);
		return chunkPtr;
	}

	CmrDisk disk_{"/tmp/registration-sweep-unittest/", "/tmp/registration-sweep-unittest/", false,
	              false};
};

TEST_F(RegistrationSweepTests, LockedChunkIsRetriedWithoutWaiting) {
	IChunk *lockedChunk = addChunk(ChunkState::Locked);
	ASSERT_NE(lockedChunk, nullptr);

	hddRegistrationSweepBegin();
	std::vector<ChunkWithVersionAndType> bulk;
	auto sweep =
	    std::async(std::launch::async, [&bulk] { return hddRegistrationSweepNext(bulk, 1); });
	const auto sweepStatus = sweep.wait_for(std::chrono::seconds(1));
	EXPECT_EQ(std::future_status::ready, sweepStatus);

	// Always release the chunk before joining the worker: the old blocking
	// implementation only returns after this release.
	hddChunkRelease(lockedChunk);
	EXPECT_EQ(RegistrationSweepResult::kRetry, sweep.get());
	EXPECT_TRUE(bulk.empty());

	EXPECT_EQ(RegistrationSweepResult::kBulkReady, hddRegistrationSweepNext(bulk, 1));
	ASSERT_EQ(bulk.size(), 1U);
	EXPECT_EQ(1U, bulk.front().id);
	EXPECT_EQ(RegistrationSweepResult::kComplete, hddRegistrationSweepNext(bulk, 1));
}
