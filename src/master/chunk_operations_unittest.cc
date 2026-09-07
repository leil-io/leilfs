/*
   Copyright 2026      Leil Storage

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"

#include <array>
#include <limits>

#include <gtest/gtest.h>

#include "common/event_loop.h"
#include "errors/saunafs_error_codes.h"
#include "master/chunk_operations_in_memory.h"
#include "master/chunks.h"
#include "master/chunkserver_db.h"
#include "master/filesystem_node.h"
#include "master/filesystem_operations.h"

namespace {

class CacheTestFilesystemOperations : public FilesystemOperationsBase {
public:
	CacheTestFilesystemOperations()
	    : FilesystemOperationsBase(std::make_unique<FilesystemNodeOperationsBase>()) {}

	const Goal &getGoalDefinition(uint8_t) const override { return goal_; }

private:
	Goal goal_;
};

class HiddenReadLocations : public ChunkOperationsInMemory {
public:
	int getVersionAndLocations(uint64_t, uint32_t, uint32_t &version, uint32_t,
	                           std::vector<ChunkTypeWithAddress> &) override {
		++readCalls;
		version = 9000;
		return SAUNAFS_ERROR_CHUNKLOST;
	}

	unsigned readCalls = 0;
};

class ChunkWriteLocationsTest : public ::testing::Test {
protected:
	void SetUp() override {
		ASSERT_EQ(chunk_strinit(), 1);
		eventloop_updatetime();
	}
	void TearDown() override { chunk_unload(); }
};

class ChunkPartLocationsTest : public ChunkWriteLocationsTest {
protected:
	void SetUp() override {
		ChunkWriteLocationsTest::SetUp();
		previousFilesystem = std::move(gFSOperations);
		gFSOperations = std::make_unique<CacheTestFilesystemOperations>();
		chunk_invalidate_goal_cache();
		previousSignal = std::move(gChunkChangedSignal);
		gChunkChangedSignal = {};
		gChunkChangedSignal.connect(
		    [this](uint64_t, uint32_t, uint32_t, uint32_t) { ++notifications; });
		for (size_t index = 0; index < servers.size(); ++index) {
			servers[index].csid = static_cast<uint16_t>(index + 1);
			servers[index].eptr = reinterpret_cast<matocsserventry *>(&handles[index]);
			previous[index] = gIdToCSEntry[index + 1];
			gIdToCSEntry[index + 1] = &servers[index];
		}
		chunk_create_with_goal_counters(17, 1, {}, 0, 0);
	}

	void TearDown() override {
		ChunkWriteLocationsTest::TearDown();
		chunk_invalidate_goal_cache();
		gFSOperations = std::move(previousFilesystem);
		gChunkChangedSignal = std::move(previousSignal);
		for (size_t index = 0; index < servers.size(); ++index) {
			gIdToCSEntry[index + 1] = previous[index];
		}
	}

	std::array<csdbentry, 2> servers;
	std::array<csdbentry *, 2> previous{};
	std::array<int, 2> handles{};
	Signal<uint64_t, uint32_t, uint32_t, uint32_t> previousSignal;
	unsigned notifications = 0;
	std::unique_ptr<IFilesystemOperations> previousFilesystem;
};

}  // namespace

TEST_F(ChunkWriteLocationsTest, UsesMutationStateInsteadOfReadOverride) {
	chunk_create_with_goal_counters(17, 7, {}, 0, 0);
	HiddenReadLocations operations;
	IChunkOperations &interface = operations;
	std::vector<ChunkTypeWithAddress> locations;
	uint32_t version = 0;

	EXPECT_EQ(interface.getWriteVersionAndLocations(17, 0, version, 100, locations),
	          SAUNAFS_STATUS_OK);
	EXPECT_EQ(version, 7U);
	EXPECT_TRUE(locations.empty());
	EXPECT_EQ(operations.readCalls, 0U);
}

TEST_F(ChunkWriteLocationsTest, MissingChunkRemainsAnError) {
	ChunkOperationsInMemory operations;
	std::vector<ChunkTypeWithAddress> locations;
	uint32_t version = 0;

	EXPECT_EQ(operations.getWriteVersionAndLocations(17, 0, version, 100, locations),
	          SAUNAFS_ERROR_NOCHUNK);
	EXPECT_EQ(version, 0U);
	EXPECT_TRUE(locations.empty());
}

TEST_F(ChunkPartLocationsTest, MaintenanceSnapshotsDoNotPopulateRegistryOrNotify) {
	const auto before = chunk_count();
	unsigned commands = 0;
	const ChunkMaintenanceSink sink = [&](const ChunkMaintenanceCommand &) {
		++commands;
		return true;
	};
	for (uint64_t chunkid = 100; chunkid < 200; ++chunkid) {
		ASSERT_EQ(chunk_run_maintenance(chunkid, 7, {}, {{2, 1}}, sink), SAUNAFS_STATUS_OK);
		EXPECT_FALSE(chunk_exists(chunkid));
		EXPECT_EQ(chunk_count(), before);
	}
	EXPECT_EQ(commands, 0U);
	EXPECT_EQ(notifications, 0U);
}

TEST_F(ChunkPartLocationsTest, MaintenanceRejectsInvalidSnapshotAndBusyLocalState) {
	const ChunkMaintenanceSink sink = [](const ChunkMaintenanceCommand &) { return true; };
	EXPECT_EQ(chunk_run_maintenance(18, 0, {}, {}, sink), SAUNAFS_ERROR_EINVAL);
	EXPECT_EQ(chunk_run_maintenance(18, 1, {{nullptr, {}}}, {}, sink), SAUNAFS_ERROR_EINVAL);
	EXPECT_EQ(chunk_run_maintenance(18, 1, {}, {{2, 0}}, sink), SAUNAFS_ERROR_EINVAL);
	EXPECT_EQ(chunk_run_maintenance(18, 1, {}, {}, {}), SAUNAFS_ERROR_EINVAL);
	ASSERT_EQ(chunk_replace_part_locations(17, 1, 19, eventloop_time() + 100, {}, 255, {}, false),
	          SAUNAFS_STATUS_OK);
	EXPECT_EQ(chunk_run_maintenance(17, 2, {}, {}, sink), SAUNAFS_ERROR_LOCKED);
	EXPECT_FALSE(chunk_exists(18));
}

TEST_F(ChunkPartLocationsTest, ReplacesStalePartsAndVersion) {
	ASSERT_EQ(chunk_replace_part_locations(17, 2, 0, 0, {{&servers[0], {}}}, 255, {}, false),
	          SAUNAFS_STATUS_OK);
	ASSERT_EQ(chunk_replace_part_locations(17, 3, 11, 0, {{&servers[1], {}}}, 255, {}, false),
	          SAUNAFS_STATUS_OK);
	std::vector<ChunkPartLocation> parts;
	uint32_t version = 0;
	ASSERT_EQ(chunk_get_publishable_parts(17, version, parts, 255), SAUNAFS_STATUS_OK);
	EXPECT_EQ(version, 3U);
	ASSERT_EQ(parts.size(), 1U);
	EXPECT_EQ(parts[0].server, &servers[1]);
	EXPECT_EQ(parts[0].partType, ChunkPartType{});
	EXPECT_EQ(chunk_can_unlock(17, 11), SAUNAFS_STATUS_OK);
}

TEST_F(ChunkPartLocationsTest, RejectsBadInputWithoutChangingCurrentState) {
	ASSERT_EQ(chunk_replace_part_locations(17, 2, 0, 0, {{&servers[0], {}}}, 255, {}, false),
	          SAUNAFS_STATUS_OK);
	EXPECT_EQ(chunk_replace_part_locations(17, 3, 0, 0, {{nullptr, {}}}, 255, {}, false),
	          SAUNAFS_ERROR_EINVAL);
	EXPECT_EQ(chunk_replace_part_locations(17, 3, 0, 0, {{&servers[1], {}}, {&servers[1], {}}}, 255,
	                                       {}, false),
	          SAUNAFS_ERROR_EINVAL);
	EXPECT_EQ(chunk_replace_part_locations(17, 3, 0, 0, {{&servers[1], ChunkPartType(1)}}, 255, {},
	                                       false),
	          SAUNAFS_ERROR_EINVAL);
	EXPECT_EQ(chunk_replace_part_locations(17, 3, 0, 0, {{&servers[1], {}}}, 0, {}, false),
	          SAUNAFS_ERROR_EINVAL);
	servers[1].eptr = nullptr;
	EXPECT_EQ(chunk_replace_part_locations(17, 3, 0, 0, {{&servers[1], {}}}, 255, {}, false),
	          SAUNAFS_ERROR_EINVAL);
	std::vector<ChunkPartLocation> parts;
	uint32_t version = 0;
	ASSERT_EQ(chunk_get_publishable_parts(17, version, parts, 255), SAUNAFS_STATUS_OK);
	EXPECT_EQ(version, 2U);
	ASSERT_EQ(parts.size(), 1U);
	EXPECT_EQ(parts[0].server, &servers[0]);
}

TEST_F(ChunkPartLocationsTest, SnapshotRejectsOverflowAndOmitsUnavailableParts) {
	ASSERT_EQ(chunk_replace_part_locations(17, 2, 0, 0, {{&servers[0], {}}, {&servers[1], {}}}, 255,
	                                       {}, false),
	          SAUNAFS_STATUS_OK);
	std::vector<ChunkPartLocation> parts;
	uint32_t version = 0;
	EXPECT_EQ(chunk_get_publishable_parts(17, version, parts, 1), SAUNAFS_ERROR_EINVAL);
	EXPECT_TRUE(parts.empty());
	servers[1].eptr = nullptr;
	ASSERT_EQ(chunk_get_publishable_parts(17, version, parts, 1), SAUNAFS_STATUS_OK);
	ASSERT_EQ(parts.size(), 1U);
	EXPECT_EQ(parts[0].server, &servers[0]);
	ASSERT_EQ(chunk_set_version(17, 3), SAUNAFS_STATUS_OK);
	ASSERT_EQ(chunk_get_publishable_parts(17, version, parts, 255), SAUNAFS_STATUS_OK);
	EXPECT_TRUE(parts.empty());
	EXPECT_EQ(version, 3U);
}

TEST_F(ChunkPartLocationsTest, DoesNotReplaceOrPublishLockedState) {
	ASSERT_EQ(chunk_replace_part_locations(17, 2, 11, std::numeric_limits<uint32_t>::max(),
	                                       {{&servers[0], {}}}, 255, {}, false),
	          SAUNAFS_STATUS_OK);
	EXPECT_EQ(chunk_replace_part_locations(17, 3, 0, 0, {{&servers[1], {}}}, 255, {}, false),
	          SAUNAFS_ERROR_LOCKED);
	std::vector<ChunkPartLocation> parts;
	uint32_t version = 0;
	EXPECT_EQ(chunk_get_publishable_parts(17, version, parts, 255), SAUNAFS_ERROR_CHUNKBUSY);
	EXPECT_TRUE(parts.empty());
	EXPECT_EQ(chunk_can_unlock(17, 11), SAUNAFS_STATUS_OK);
}

TEST_F(ChunkPartLocationsTest, RefreshIsSilentAndCanRestoreOlderOrEmptyState) {
	ASSERT_EQ(chunk_replace_part_locations(17, 7, 0, 0, {{&servers[0], {}}}, 255, {}, false),
	          SAUNAFS_STATUS_OK);
	ASSERT_EQ(chunk_replace_part_locations(17, 6, 0, 0, {}, 255, {}, false), SAUNAFS_STATUS_OK);
	std::vector<ChunkPartLocation> parts;
	uint32_t version = 0;
	ASSERT_EQ(chunk_get_publishable_parts(17, version, parts, 255), SAUNAFS_STATUS_OK);
	EXPECT_EQ(version, 6U);
	EXPECT_TRUE(parts.empty());
	EXPECT_EQ(notifications, 0U);
}

TEST_F(ChunkPartLocationsTest, RejectsUnregisteredAndAliasedHandles) {
	servers[0].csid = 0;
	EXPECT_EQ(chunk_replace_part_locations(17, 2, 0, 0, {{&servers[0], {}}}, 255, {}, false),
	          SAUNAFS_ERROR_EINVAL);
	servers[0].csid = csdbentry::kMaxIdCount;
	EXPECT_EQ(chunk_replace_part_locations(17, 2, 0, 0, {{&servers[0], {}}}, 255, {}, false),
	          SAUNAFS_ERROR_EINVAL);
	servers[0].csid = 1;
	csdbentry alias = servers[0];
	EXPECT_EQ(chunk_replace_part_locations(17, 2, 0, 0, {{&alias, {}}}, 255, {}, false),
	          SAUNAFS_ERROR_EINVAL);
	EXPECT_EQ(chunk_replace_part_locations(17, 0, 0, 0, {}, 255, {}, false), SAUNAFS_ERROR_EINVAL);
	EXPECT_EQ(notifications, 0U);
}

TEST_F(ChunkPartLocationsTest, RefreshesReferenceCountsAndGoalStatisticsSilently) {
	ASSERT_EQ(chunk_replace_part_locations(17, 2, 0, 0, {}, 255, {{1, 1}}, false),
	          SAUNAFS_STATUS_OK);
	const auto &stats = chunk_get_availability_state();
	const auto total = [&stats](uint8_t goal) {
		return stats.safeChunks(goal) + stats.endangeredChunks(goal) + stats.lostChunks(goal);
	};
	EXPECT_EQ(total(1), 1U);
	ASSERT_EQ(chunk_replace_part_locations(17, 3, 0, 0, {}, 255, {{2, 2}}, false),
	          SAUNAFS_STATUS_OK);
	uint32_t version = 0;
	ChunkGoalCounters counters;
	ASSERT_TRUE(chunk_get_version_and_goal_counters(17, version, counters));
	EXPECT_EQ(version, 3U);
	EXPECT_EQ(counters.fileCount(), 2U);
	EXPECT_EQ(counters.highestIdGoal(), 2U);
	EXPECT_EQ(total(1), 0U);
	EXPECT_EQ(total(2), 1U);
	EXPECT_EQ(notifications, 0U);
}

TEST_F(ChunkPartLocationsTest, InvalidGoalsLeaveAllCachedStateUnchanged) {
	ASSERT_EQ(chunk_replace_part_locations(17, 2, 0, 0, {{&servers[0], {}}}, 255, {{1, 2}}, false),
	          SAUNAFS_STATUS_OK);
	for (const auto &goals : std::vector<std::vector<ChunkGoalCounters::GoalCounter>>{
	         {{1, 0}}, {{0, 1}}, {{2, 1}, {1, 1}}}) {
		EXPECT_EQ(
		    chunk_replace_part_locations(17, 3, 11, 0, {{&servers[1], {}}}, 255, goals, false),
		    SAUNAFS_ERROR_EINVAL);
	}
	uint32_t version = 0;
	ChunkGoalCounters counters;
	ASSERT_TRUE(chunk_get_version_and_goal_counters(17, version, counters));
	EXPECT_EQ(version, 2U);
	EXPECT_EQ(counters.fileCount(), 2U);
	EXPECT_EQ(counters.highestIdGoal(), 1U);
	std::vector<ChunkPartLocation> parts;
	ASSERT_EQ(chunk_get_publishable_parts(17, version, parts, 255), SAUNAFS_STATUS_OK);
	ASSERT_EQ(parts.size(), 1U);
	EXPECT_EQ(parts[0].server, &servers[0]);
	uint32_t lockid = 99;
	uint32_t lockedto = 99;
	ASSERT_TRUE(chunk_get_lock_state(17, lockid, lockedto));
	EXPECT_EQ(lockid, 0U);
	EXPECT_EQ(lockedto, 0U);
	EXPECT_EQ(notifications, 0U);
}

TEST(ChunkGoalCounters, RestoresOverflowEntriesWithoutExpandingReferences) {
	const std::vector<ChunkGoalCounters::GoalCounter> entries{{1, 3}, {1, 255}, {2, 2}};
	ChunkGoalCounters counters(entries);
	EXPECT_EQ(counters.fileCount(), 260U);
	EXPECT_EQ(counters.size(), 3U);
	EXPECT_TRUE(std::equal(counters.begin(), counters.end(), entries.begin(), entries.end()));
	counters.removeFile(1);
	EXPECT_EQ(counters.fileCount(), 259U);
	counters.addFile(2);
	EXPECT_EQ(counters.fileCount(), 260U);
	EXPECT_EQ(counters.highestIdGoal(), 2U);
	EXPECT_EQ(ChunkGoalCounters(std::span<const ChunkGoalCounters::GoalCounter>{}).size(), 0U);
}

TEST(ChunkGoalCounters, RejectsInvalidRestoredEntriesAndOverflow) {
	for (const auto &entries : std::vector<std::vector<ChunkGoalCounters::GoalCounter>>{
	         {{0, 1}}, {{1, 0}}, {{2, 1}, {1, 1}}}) {
		EXPECT_THROW(ChunkGoalCounters{entries}, ChunkGoalCounters::InvalidOperation);
	}
	const size_t limit = ChunkGoalCounters::Counters{}.max_size();
	const std::vector<ChunkGoalCounters::GoalCounter> entries(limit + 1, {1, 255});
	EXPECT_THROW(ChunkGoalCounters{entries}, ChunkGoalCounters::InvalidOperation);
}
