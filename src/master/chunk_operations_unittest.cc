/*
   Copyright 2026      Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <limits>
#include <utility>

#include <gtest/gtest.h>

#include "common/event_loop.h"
#include "common/saunafs_version.h"
#include "config/cfg.h"
#include "errors/leil_error_codes.h"
#include "master/chunk_operations_in_memory.h"
#include "master/chunks.h"
#include "master/chunkserver_db.h"
#include "master/filesystem_node.h"
#include "master/filesystem_operation_context.h"
#include "master/filesystem_operations.h"
#include "master/goal_config_loader.h"
#include "master/matocsserv.h"

// Check the apply precondition without sending chunkserver commands.
bool chunk_mutation_plan_current(const ChunkMutationPlan &plan);

namespace {

class CacheTestFilesystemOperations : public FilesystemOperationsBase {
public:
	explicit CacheTestFilesystemOperations(Goal goal = {})
	    : FilesystemOperationsBase(std::make_unique<FilesystemNodeOperationsBase>()),
	      goal_(std::move(goal)) {}

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
			connect(index);
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
		for (auto *connection : connections) { matocsserv_test_connection_free(connection); }
	}

	// A fresh connection at the server's address, as a chunkserver reconnecting gets one.
	void connect(size_t index) {
		matocsserv_test_connection_free(connections[index]);
		connections[index] =
		    matocsserv_test_connection(&servers[index], 0x7f000001 + static_cast<uint32_t>(index),
		                               9422, kFirstVersionWithChunkserverSideChunkLock);
	}

	std::array<csdbentry, 3> servers;
	std::array<csdbentry *, 3> previous{};
	std::array<matocsserventry *, 3> connections{};
	Signal<uint64_t, uint32_t, uint32_t, uint32_t> previousSignal;
	unsigned notifications = 0;
	std::unique_ptr<IFilesystemOperations> previousFilesystem;
};

class ChunkMutationPlanTest : public ChunkPartLocationsTest {
protected:
	void install(const std::vector<ChunkPartLocation> &parts, bool needsBump = false) {
		ASSERT_EQ(chunk_replace_part_locations(17, 3, 0, 0, parts, 255, {{1, 1}}, needsBump),
		          SAUNAFS_STATUS_OK);
	}
};

class ChunkserverSideLockPlanTest : public ChunkMutationPlanTest {
protected:
	void SetUp() override {
		// chunk_strinit, run by the base fixture, reads the switch from the configuration
		std::ofstream(configPath) << "USE_CHUNKSERVER_SIDE_CHUNK_LOCK = 1\n";
		ASSERT_EQ(cfg_load(configPath.c_str(), 0), 0);
		ASSERT_TRUE(cfg_isdefined("USE_CHUNKSERVER_SIDE_CHUNK_LOCK"));
		ChunkMutationPlanTest::SetUp();
	}
	void TearDown() override {
		ChunkMutationPlanTest::TearDown();
		cfg_term();
		std::remove(configPath.c_str());
	}
	const std::string configPath = ::testing::TempDir() + "chunkserver_side_lock.cfg";
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

TEST_F(ChunkWriteLocationsTest, ReclaimStopsAtBudgetInsideOneBucket) {
	// The chunk table hashes on the low 22 bits, so these three share a bucket.
	constexpr uint64_t kBucketStride = uint64_t{1} << 22;
	for (uint64_t chunkid : {uint64_t{17}, 17 + kBucketStride, 17 + 2 * kBucketStride}) {
		chunk_create_with_goal_counters(chunkid, 1, {}, 0, 0);
	}
	unsigned asked = 0;
	const auto hasRecord = [&](uint64_t) {
		++asked;
		return true;
	};

	EXPECT_EQ(chunk_reclaim_unreferenced(2, hasRecord), 2U);
	EXPECT_EQ(asked, 2U);
	EXPECT_EQ(chunk_reclaim_unreferenced(2, hasRecord), 1U);
	EXPECT_EQ(asked, 3U);
	// The pass wrapped around the table, so the next call starts the bucket over.
	EXPECT_EQ(chunk_reclaim_unreferenced(2, hasRecord), 2U);
	EXPECT_EQ(asked, 5U);
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

TEST_F(ChunkWriteLocationsTest, SynchronousWritesNeedNoDeferredCompletion) {
	ChunkOperationsInMemory operations;
	FilesystemOperationContext context;
	EXPECT_EQ(operations.writeStartStatus(context, 17), nullptr);
}

TEST_F(ChunkPartLocationsTest, MutationSnapshotAllowsLocksAndRejectsOverflowAndOldVersions) {
	const auto expiry = static_cast<uint32_t>(eventloop_time()) + 120;
	ASSERT_EQ(chunk_replace_part_locations(17, 4, 77, expiry,
	                                       {{&servers[0], {}}, {&servers[1], {}}}, 255, {}, false),
	          SAUNAFS_STATUS_OK);
	std::vector<ChunkPartLocation> parts;
	ASSERT_EQ(chunk_get_mutation_parts(17, 4, parts, 255), SAUNAFS_STATUS_OK);
	ASSERT_EQ(parts.size(), 2U);
	EXPECT_EQ(chunk_get_mutation_parts(17, 4, parts, 1), SAUNAFS_ERROR_EINVAL);
	EXPECT_TRUE(parts.empty());
	EXPECT_EQ(chunk_get_mutation_parts(17, 3, parts, 255), SAUNAFS_ERROR_CHUNKLOST);
	EXPECT_TRUE(parts.empty());
	servers[1].eptr = nullptr;
	ASSERT_EQ(chunk_get_mutation_parts(17, 4, parts, 255), SAUNAFS_STATUS_OK);
	ASSERT_EQ(parts.size(), 1U);
	EXPECT_EQ(parts.front().server, &servers[0]);
}

TEST_F(ChunkMutationPlanTest, AddedParityPartIsNotTruncatedByACommittedPlan) {
	install({{&servers[0], {}}});
	const auto plan = chunk_plan_truncate(17, 77, 1000, 1, true, false);
	ASSERT_EQ(plan.status, SAUNAFS_STATUS_OK);
	ASSERT_TRUE(chunk_mutation_plan_current(plan));
	const ChunkPartType parity(slice_traits::ec::getSliceType(3, 1), 3);
	install({{&servers[0], {}}, {&servers[1], parity}});
	uint8_t operation = 255;
	ASSERT_TRUE(chunk_apply_mutation_plan(plan, operation));
	EXPECT_TRUE(chunk_operation_waits_for_replies(operation));
	std::vector<ChunkPartLocation> parts;
	ASSERT_EQ(chunk_get_mutation_parts(17, plan.targetVersion, parts, 255), SAUNAFS_STATUS_OK);
	ASSERT_EQ(parts.size(), 1U);
	EXPECT_EQ(parts.front().server, &servers[0]);
}

TEST_F(ChunkMutationPlanTest, LostPartKeepsAVersionIncreasePlanForTheSurvivors) {
	install({{&servers[0], {}}, {&servers[1], {}}}, true);
	const auto plan = chunk_plan_modify(17, 77, 1, false, 0);
	ASSERT_EQ(plan.status, SAUNAFS_STATUS_OK);
	ASSERT_GT(plan.targetVersion, plan.chunkVersion);
	ASSERT_TRUE(chunk_mutation_plan_current(plan));
	install({{&servers[0], {}}}, true);
	uint8_t operation = 255;
	ASSERT_TRUE(chunk_apply_mutation_plan(plan, operation));
	EXPECT_TRUE(chunk_operation_waits_for_replies(operation));
	std::vector<ChunkPartLocation> parts;
	ASSERT_EQ(chunk_get_mutation_parts(17, plan.targetVersion, parts, 255), SAUNAFS_STATUS_OK);
	ASSERT_EQ(parts.size(), 1U);
	EXPECT_EQ(parts.front().server, &servers[0]);
}

TEST_F(ChunkMutationPlanTest, VersionMoveRefusesUnreadableSurvivorsDespiteAnArrivedPart) {
	gFSOperations = std::make_unique<CacheTestFilesystemOperations>(
	    goal_config::parseLine("1 xor2: $xor2").second);
	chunk_invalidate_goal_cache();
	const auto firstPart = slice_traits::xors::ChunkPartType(2, 1);
	const auto secondPart = slice_traits::xors::ChunkPartType(2, 2);

	for (bool truncate : {false, true}) {
		SCOPED_TRACE(truncate);
		install({{&servers[0], firstPart}, {&servers[1], secondPart}}, true);
		const auto plan = truncate ? chunk_plan_truncate(17, 77, 1000, 1, false, false)
		                           : chunk_plan_modify(17, 77, 1, false, 0);
		ASSERT_EQ(plan.status, SAUNAFS_STATUS_OK);
		ASSERT_TRUE(chunk_mutation_plan_current(plan));

		// The replacement restores readability but would stay at the old version.
		install({{&servers[0], firstPart}, {&servers[2], secondPart}}, true);
		ASSERT_EQ(chunk_plan_modify(17, 77, 1, false, 0).status, SAUNAFS_STATUS_OK);
		uint8_t operation = 255;
		ASSERT_FALSE(chunk_apply_mutation_plan(plan, operation));
		std::vector<ChunkPartLocation> parts;
		ASSERT_EQ(chunk_get_mutation_parts(17, plan.chunkVersion, parts, 255), SAUNAFS_STATUS_OK);
		EXPECT_EQ(parts.size(), 2U);
	}
}

TEST_F(ChunkMutationPlanTest, AddedPartIsNotAddressedByACommittedPlan) {
	install({{&servers[0], {}}}, true);
	const auto plan = chunk_plan_modify(17, 77, 1, false, 0);
	ASSERT_EQ(plan.status, SAUNAFS_STATUS_OK);
	ASSERT_GT(plan.targetVersion, plan.chunkVersion);
	install({{&servers[0], {}}, {&servers[1], {}}}, true);
	uint8_t operation = 255;
	ASSERT_TRUE(chunk_apply_mutation_plan(plan, operation));
	EXPECT_TRUE(chunk_operation_waits_for_replies(operation));
	std::vector<ChunkPartLocation> parts;
	ASSERT_EQ(chunk_get_mutation_parts(17, plan.targetVersion, parts, 255), SAUNAFS_STATUS_OK);
	ASSERT_EQ(parts.size(), 1U);
	EXPECT_EQ(parts.front().server, &servers[0]);
}

TEST_F(ChunkMutationPlanTest, EveryPlannedPartGoneInvalidatesAVersionIncreasePlan) {
	install({{&servers[0], {}}}, true);
	const auto plan = chunk_plan_modify(17, 77, 1, false, 0);
	ASSERT_EQ(plan.status, SAUNAFS_STATUS_OK);
	ASSERT_GT(plan.targetVersion, plan.chunkVersion);
	install({{&servers[1], {}}}, true);
	EXPECT_FALSE(chunk_mutation_plan_current(plan));
}

TEST_F(ChunkMutationPlanTest, ReusedRegistryIdAtAnotherAddressInvalidatesAPlan) {
	install({{&servers[0], {}}});
	const auto plan = chunk_plan_modify(17, 77, 1, false, 0);
	ASSERT_EQ(plan.status, SAUNAFS_STATUS_OK);
	ASSERT_TRUE(chunk_mutation_plan_current(plan));
	servers[1].csid = servers[0].csid;
	servers[0].eptr = servers[1].eptr;
	EXPECT_FALSE(chunk_mutation_plan_current(plan));
}

TEST_F(ChunkMutationPlanTest, NonePlanRefusesANewVersionIncreaseRequirement) {
	install({{&servers[0], {}}});
	const auto plan = chunk_plan_modify(17, 77, 1, false, 0);
	ASSERT_EQ(plan.status, SAUNAFS_STATUS_OK);
	ASSERT_EQ(plan.operation, 0U);
	ASSERT_TRUE(chunk_mutation_plan_current(plan));
	install({{&servers[0], {}}}, true);
	EXPECT_FALSE(chunk_mutation_plan_current(plan));
}

TEST_F(ChunkMutationPlanTest, PartOrderAndImmediateInMemoryApplyRemainValid) {
	install({{&servers[0], {}}, {&servers[1], {}}});
	const auto plan = chunk_plan_modify(17, 77, 1, false, 0);
	ASSERT_EQ(plan.status, SAUNAFS_STATUS_OK);
	install({{&servers[1], {}}, {&servers[0], {}}});
	EXPECT_TRUE(chunk_mutation_plan_current(plan));
	uint8_t operation = 255;
	EXPECT_TRUE(chunk_apply_mutation_plan(plan, operation));
	EXPECT_EQ(operation, 0U);
	EXPECT_EQ(chunk_can_unlock(17, 77), SAUNAFS_STATUS_OK);
}

TEST_F(ChunkMutationPlanTest, SnapshotReferenceKeepsACommittedInPlacePlan) {
	install({{&servers[0], {}}});
	const auto writePlan = chunk_plan_modify(17, 77, 1, false, 0);
	const auto truncatePlan = chunk_plan_truncate(17, 77, 1000, 1, false, false);
	ASSERT_EQ(writePlan.status, SAUNAFS_STATUS_OK);
	ASSERT_EQ(truncatePlan.status, SAUNAFS_STATUS_OK);
	ASSERT_EQ(truncatePlan.targetChunkId, 17U);

	ASSERT_EQ(chunk_add_file(17, 1), SAUNAFS_STATUS_OK);
	EXPECT_TRUE(chunk_mutation_plan_current(truncatePlan));
	uint8_t operation = 255;
	ASSERT_TRUE(chunk_apply_mutation_plan(writePlan, operation));
	EXPECT_EQ(operation, 0U);
	EXPECT_EQ(chunk_can_unlock(17, 77), SAUNAFS_STATUS_OK);
}

TEST_F(ChunkMutationPlanTest, VersionMovesInvalidateUnplannedOldCopies) {
	for (bool truncate : {false, true}) {
		ASSERT_EQ(chunk_unlock(17), SAUNAFS_STATUS_OK);
		install({{&servers[0], {}}}, true);
		const auto plan = truncate ? chunk_plan_truncate(17, 77, 1000, 1, false, false)
		                           : chunk_plan_modify(17, 77, 1, false, 0);
		ASSERT_EQ(plan.status, SAUNAFS_STATUS_OK);
		install({{&servers[0], {}}, {&servers[1], {}}}, true);
		uint8_t operation = 255;
		ASSERT_TRUE(chunk_apply_mutation_plan(plan, operation));
		chunk_got_setversion_status(servers[0].eptr, 17, {}, SAUNAFS_STATUS_OK);

		// Only the planned copy moved; the arrived copy must disappear from write retries too.
		uint32_t version = 0;
		std::vector<ChunkTypeWithAddress> locations;
		ChunkOperationsInMemory operations;
		ASSERT_EQ(operations.getWriteVersionAndLocations(17, 0, version, 255, locations),
		          SAUNAFS_STATUS_OK);
		EXPECT_EQ(version, 4U);
		EXPECT_EQ(locations.size(), 1U);
		const auto reentry = chunk_plan_modify(17, 77, 1, false, 0);
		ASSERT_EQ(reentry.status, SAUNAFS_STATUS_OK);
		EXPECT_EQ(reentry.parts.size(), 1U);
	}
}

TEST(ChunkserverRegistryTest, RemovedIdDoesNotExposeTheFreeList) {
	constexpr uint32_t address = 0x7f001234;
	constexpr uint16_t port = 12345;
	auto *connection = matocsserv_test_connection(nullptr, address, port, 0);
	ASSERT_EQ(csdb_new_connection(address, port, connection), 1);
	const auto id = csdb_find(address, port)->csid;
	csdb_lost_connection(address, port);
	ASSERT_EQ(csdb_remove_server(address, port), 1);
	EXPECT_EQ(csdb_find(address, port), nullptr);
	EXPECT_EQ(csdb_find(id), nullptr);
	matocsserv_test_connection_free(connection);
}

TEST_F(ChunkMutationPlanTest, AddedPartInvalidatesADeferredLockGrant) {
	install({{&servers[0], {}}});
	const auto plan = chunk_plan_modify(17, 77, 1, false, 0);
	ASSERT_EQ(plan.status, SAUNAFS_STATUS_OK);
	ASSERT_EQ(plan.operation, 0U);
	install({{&servers[0], {}}, {&servers[1], {}}});
	EXPECT_FALSE(chunk_mutation_plan_current(plan));
}

TEST_F(ChunkserverSideLockPlanTest, WriteEndBetweenPlanAndApplyInvalidatesANonePlan) {
	install({{&servers[0], {}}});
	// The first grant locks the part on its chunkserver, which confirms.
	const auto grant = chunk_plan_modify(17, 77, 1, false, 0);
	ASSERT_EQ(grant.status, SAUNAFS_STATUS_OK);
	ASSERT_EQ(grant.operation, 6U);
	uint8_t operation = 255;
	ASSERT_TRUE(chunk_apply_mutation_plan(grant, operation));
	chunk_got_chunklock_status(servers[0].eptr, 17, {}, SAUNAFS_STATUS_OK);

	// A renewal under the same lock finds every part being written and plans no lock.
	const auto renewal = chunk_plan_modify(17, 77, 1, false, 0);
	ASSERT_EQ(renewal.status, SAUNAFS_STATUS_OK);
	ASSERT_EQ(renewal.operation, 0U);

	// The client's write end reaches the chunkserver before the renewal applies: the part is
	// idle again and the grant would leave it without a lock.
	chunk_got_writeend_status(servers[0].eptr, 17, {}, SAUNAFS_STATUS_OK);
	EXPECT_FALSE(chunk_mutation_plan_current(renewal));
}

TEST_F(ChunkMutationPlanTest, RemovedRegistryEntryIsDroppedFromTheChunk) {
	constexpr uint32_t address = 0x7f001235;
	constexpr uint16_t port = 12346;
	// The fixture's manual entry must not replace a link used by the real id allocator.
	gIdToCSEntry[servers[0].csid] = previous[0];
	ASSERT_EQ(csdb_new_connection(address, port, servers[0].eptr), 1);
	auto *registered = csdb_find(address, port);
	ASSERT_NE(registered, nullptr);
	ASSERT_EQ(registered->csid, servers[0].csid);
	ASSERT_EQ(chunk_replace_part_locations(17, 3, 0, 0, {{registered, {}}}, 255, {{1, 1}}, false),
	          SAUNAFS_STATUS_OK);
	const auto plan = chunk_plan_modify(17, 77, 1, false, 0);
	ASSERT_EQ(plan.status, SAUNAFS_STATUS_OK);
	ASSERT_EQ(plan.operation, 0U);
	ASSERT_EQ(plan.parts.size(), 1U);
	csdb_lost_connection(address, port);
	ASSERT_EQ(csdb_remove_server(address, port), 1);

	// A different lookup bypasses the last-chunk shortcut and runs disconnected-copy cleanup.
	chunk_create_with_goal_counters(18, 1, {}, 0, 0);
	ASSERT_TRUE(chunk_exists(18));
	EXPECT_FALSE(chunk_mutation_plan_current(plan));
}

TEST_F(ChunkMutationPlanTest, DiscoveryBlocksVersionMoveBeforeAnyMutation) {
	install({{&servers[0], {}}}, true);
	ChunkMutationDiscovery discovery;
	FilesystemOperationContext context;
	context.setChunkMutationCheck([&](const ChunkMutationPlan &plan) {
		return discovery.requiresQuery(plan, true, 9) ? SAUNAFS_ERROR_NOCHUNKSERVERS
		                                              : SAUNAFS_STATUS_OK;
	});
	ChunkOperationsInMemory operations;
	uint32_t lockId = 0;
	uint64_t target = 0;
	uint8_t operation = 0;

	EXPECT_EQ(operations.multiModify(context, 17, &lockId, 1, false, &operation, &target, 0),
	          SAUNAFS_ERROR_NOCHUNKSERVERS);
	EXPECT_EQ(target, 17U);
	EXPECT_EQ(lockId, 0U);
	EXPECT_EQ(notifications, 0U);
	EXPECT_TRUE(chunk_needs_version_increase(17));
	std::vector<ChunkPartLocation> parts;
	ASSERT_EQ(chunk_get_mutation_parts(17, 3, parts, 255), SAUNAFS_STATUS_OK);
	ASSERT_EQ(parts.size(), 1U);

	// A timed-out wait is not permission to execute the previously refused plan.
	discovery.complete = false;
	EXPECT_EQ(operations.multiModify(context, 17, &lockId, 1, false, &operation, &target, 0),
	          SAUNAFS_ERROR_NOCHUNKSERVERS);
	EXPECT_EQ(notifications, 0U);
}

TEST_F(ChunkMutationPlanTest, CompletedDiscoveryReplansWithTheNewlyFoundCopy) {
	install({{&servers[0], {}}}, true);
	ChunkMutationDiscovery discovery;
	const auto initial = chunk_plan_modify(17, 0, 1, false, 0);
	ASSERT_TRUE(discovery.requiresQuery(initial, true, 9));

	// The query finds a second copy before either copy changes version.
	chunk_server_has_chunk(connections[1], 17, 3, {});
	discovery.complete = true;
	FilesystemOperationContext context;
	context.setChunkMutationCheck([&](const ChunkMutationPlan &plan) {
		return discovery.requiresQuery(plan, true, 9) ? SAUNAFS_ERROR_NOCHUNKSERVERS
		                                              : SAUNAFS_STATUS_OK;
	});
	ChunkOperationsInMemory operations;
	uint32_t lockId = 0;
	uint64_t target = 0;
	uint8_t operation = 0;
	ASSERT_EQ(operations.multiModify(context, 17, &lockId, 1, false, &operation, &target, 0),
	          SAUNAFS_STATUS_OK);
	for (unsigned index : {0U, 1U}) {
		ChunkPartState part;
		ASSERT_TRUE(chunk_get_part_state(connections[index], 17, {}, part));
		EXPECT_EQ(part.chunkVersion, 4U);
		EXPECT_EQ(part.partVersion, 4U);
		EXPECT_TRUE(part.busy);
	}
	EXPECT_TRUE(chunk_operation_waits_for_replies(operation));
}

TEST_F(ChunkMutationPlanTest, DiscoveryRejectsChangedChunkVersionOrConnections) {
	install({{&servers[0], {}}}, true);
	const auto original = chunk_plan_modify(17, 0, 1, false, 0);
	for (unsigned change = 0; change != 3; ++change) {
		ChunkMutationDiscovery discovery;
		ASSERT_TRUE(discovery.requiresQuery(original, true, 9));
		discovery.complete = true;
		auto changed = original;
		if (change == 0) { changed.chunkId = changed.targetChunkId = 18; }
		if (change == 1) {
			++changed.chunkVersion;
			++changed.targetVersion;
		}
		EXPECT_TRUE(discovery.requiresQuery(changed, true, change == 2 ? 10 : 9));
		EXPECT_FALSE(discovery.complete);
	}
}

TEST_F(ChunkMutationPlanTest, DiscoveryDoesNotGateSteadyStateOrUnchangedVersions) {
	install({{&servers[0], {}}}, true);
	const auto bump = chunk_plan_modify(17, 0, 1, false, 0);
	ChunkMutationDiscovery discovery;
	EXPECT_FALSE(discovery.requiresQuery(bump, false, 9));

	install({{&servers[0], {}}});
	const auto unchanged = chunk_plan_modify(17, 0, 1, false, 0);
	EXPECT_EQ(unchanged.targetVersion, unchanged.chunkVersion);
	EXPECT_FALSE(discovery.requiresQuery(unchanged, true, 9));

	// A new allocation has no older version to strand on unreported replicas.
	auto create = bump;
	create.chunkId = 0;
	create.targetChunkId = 18;
	EXPECT_FALSE(discovery.requiresQuery(create, true, 9));
}

TEST_F(ChunkMutationPlanTest, DiscoveryAlsoBlocksInPlaceTruncation) {
	install({{&servers[0], {}}});
	ChunkMutationDiscovery discovery;
	FilesystemOperationContext context;
	context.setChunkMutationCheck([&](const ChunkMutationPlan &plan) {
		return discovery.requiresQuery(plan, true, 9) ? SAUNAFS_ERROR_NOCHUNKSERVERS
		                                              : SAUNAFS_STATUS_OK;
	});
	ChunkOperationsInMemory operations;
	uint64_t target = 0;
	EXPECT_EQ(operations.multiTruncate(context, 17, 0, 1000, 1, false, false, &target),
	          SAUNAFS_ERROR_NOCHUNKSERVERS);
	EXPECT_EQ(notifications, 0U);
	std::vector<ChunkPartLocation> parts;
	EXPECT_EQ(chunk_get_mutation_parts(17, 3, parts, 255), SAUNAFS_STATUS_OK);
}

TEST_F(ChunkMutationPlanTest, IdentityBackendDoesNotRequireInventoryDiscovery) {
	class IdentityOperations : public ChunkOperationsInMemory {
		bool requestsChunkserverIdentity() const override { return true; }
	};
	install({{&servers[0], {}}}, true);
	auto previousOperations = std::move(gChunkOperations);
	gChunkOperations = std::make_unique<IdentityOperations>();
	ChunkMutationDiscovery discovery;
	const auto status =
	    matocsserv_check_mutation_discovery(chunk_plan_modify(17, 0, 1, false, 0), discovery);
	gChunkOperations = std::move(previousOperations);
	EXPECT_EQ(status, SAUNAFS_STATUS_OK);
	EXPECT_EQ(discovery.chunkId, 0U);
}

TEST_F(ChunkMutationPlanTest, DiscoveryFromAnUnreadablePlanAuthorizesTheReplannedWrite) {
	gFSOperations = std::make_unique<CacheTestFilesystemOperations>(goal_config::defaultGoal(1));
	chunk_invalidate_goal_cache();
	install({}, true);
	ChunkMutationDiscovery discovery;
	FilesystemOperationContext context;
	context.setChunkMutationCheck([&](const ChunkMutationPlan &plan) {
		return discovery.requiresQuery(plan, true, 9) ? SAUNAFS_ERROR_NOCHUNKSERVERS
		                                              : SAUNAFS_STATUS_OK;
	});
	ChunkOperationsInMemory operations;
	uint32_t lockId = 0;
	uint64_t target = 0;
	uint8_t operation = 0;
	ASSERT_EQ(operations.multiModify(context, 17, &lockId, 1, false, &operation, &target, 0),
	          SAUNAFS_ERROR_CHUNKLOST);
	EXPECT_EQ(discovery.chunkId, 17U);
	EXPECT_EQ(discovery.version, 3U);

	// The existing CHUNKLOST discovery must not cause an extra failed client retry.
	chunk_server_has_chunk(connections[0], 17, 3, {});
	discovery.complete = true;
	EXPECT_EQ(operations.multiModify(context, 17, &lockId, 1, false, &operation, &target, 0),
	          SAUNAFS_STATUS_OK);
}
