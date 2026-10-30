/*
   Copyright 2026      Leil Storage OÜ

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

#include <array>
#include <memory>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "common/chunk_version_with_todel_flag.h"
#include "common/event_loop.h"
#include "common/saunafs_version.h"
#include "common/slice_traits.h"
#include "errors/leil_error_codes.h"
#include "master/chunk_operations_in_memory.h"
#include "master/chunks.h"
#include "master/chunkserver_db.h"
#include "master/filesystem_node.h"
#include "master/filesystem_operations.h"
#include "master/goal_config_loader.h"
#include "master/matocsserv.h"
#include "protocol/cstoma.h"
#include "protocol/matocs.h"
#include "unittests/packet.h"

namespace {

// Own fake connections and logical time here. Narrow fixture helpers hide the
// connection layout; scheduling, replies and chunk writes use production code.
struct Access {
	using Connection =
	    std::unique_ptr<matocsserventry, decltype(&matocsserv_test::destroy_connection)>;
	inline static std::vector<Connection> servers;
	inline static SteadyTimePoint now;
	inline static std::size_t work = 0;

	static matocsserventry *connect(csdbentry &entry, uint32_t version = SAUNAFS_VERSHEX) {
		if (servers.empty()) {
			now = SteadyClock::now();
			work = 0;
		}
		Connection server(matocsserv_test::create_connection(entry, version),
		                  matocsserv_test::destroy_connection);
		servers.push_back(std::move(server));
		return entry.eptr;
	}

	static void disconnect(matocsserventry *server) {
		std::erase_if(servers, [server](const auto &entry) { return entry.get() == server; });
	}

	static std::vector<MessageBuffer> takePackets(matocsserventry *server) {
		return matocsserv_test::take_output_packets(server);
	}

	static void tick(uint32_t budgetMicroseconds = 1'000'000) {
		// Isolate work-limit assertions from host scheduling with a generous deadline.
		work = matocsserv_process_disk_state_refreshes(
		    std::chrono::microseconds(budgetMicroseconds), now);
	}

	static std::size_t lastWork() { return work; }
	static void advanceTime(uint32_t milliseconds) {
		now += std::chrono::milliseconds(milliseconds);
	}

	static void rehash(matocsserventry *server) {
		matocsserv_test::rehash_disk_state_refreshes(server);
	}

	static void fillOutput(matocsserventry *server) {
		matocsserv_test::fill_output_packets(server);
	}

	static void reply(matocsserventry *server, const MessageBuffer &payload) {
		matocsserv_sau_query_chunks(server, payload, now);
	}

	static void expire(matocsserventry *server, uint64_t chunkId) {
		matocsserv_test::expire_disk_state_refresh(server, chunkId, now);
	}

	static bool disconnectRequested(matocsserventry *server) {
		return matocsserv_is_killed(server);
	}

	static std::size_t pendingRefreshes(const matocsserventry *server) {
		return matocsserv_test::pending_disk_state_refreshes(server);
	}

	static ChunkPartState part(matocsserventry *server, uint64_t chunkId, ChunkPartType type) {
		ChunkPartState state;
		EXPECT_TRUE(chunk_get_part_state(server, chunkId, type, state));
		return state;
	}
};

class DiskStateFilesystemOperations : public FilesystemOperationsBase {
public:
	DiskStateFilesystemOperations()
	    : FilesystemOperationsBase(std::make_unique<FilesystemNodeOperationsBase>()) {}
	const Goal &getGoalDefinition(uint8_t) const override { return goal_; }

private:
	Goal goal_ = goal_config::defaultGoal(1);
};

class ChunkDiskStateTest : public ::testing::Test {
protected:
	static constexpr uint64_t kChunkId = 17;
	static constexpr uint32_t kVersion = 7;
	const ChunkPartType type = slice_traits::standard::ChunkPartType();

	void SetUp() override {
		previousFilesystem = std::move(gFSOperations);
		gFSOperations = std::make_unique<DiskStateFilesystemOperations>();
		previousOperations = std::move(gChunkOperations);
		gChunkOperations = std::make_unique<ChunkOperationsInMemory>();
		previousSignal = std::move(gChunkChangedSignal);
		gChunkChangedSignal = {};
		chunk_invalidate_goal_cache();
		ASSERT_EQ(chunk_strinit(), 1);
		eventloop_updatetime();

		entry.csid = 1;
		previousEntry = gIdToCSEntry[entry.csid];
		gIdToCSEntry[entry.csid] = &entry;
		server = Access::connect(entry);
		for (std::size_t index = 0; index < extraEntries.size(); ++index) {
			extraEntries[index].csid = static_cast<uint16_t>(index + 2);
			previousExtraEntries[index] = gIdToCSEntry[index + 2];
			gIdToCSEntry[index + 2] = &extraEntries[index];
		}
		chunk_create_with_goal_counters(kChunkId, kVersion, {{1, 1}}, 0, 0);
		report(kVersion, false);
	}

	void TearDown() override {
		chunk_unload();
		if (server != nullptr) { Access::disconnect(server); }
		for (std::size_t index = 0; index < extraEntries.size(); ++index) {
			if (extraEntries[index].eptr != nullptr) {
				Access::disconnect(extraEntries[index].eptr);
			}
			gIdToCSEntry[index + 2] = previousExtraEntries[index];
		}
		gIdToCSEntry[entry.csid] = previousEntry;
		chunk_invalidate_goal_cache();
		gChunkChangedSignal = std::move(previousSignal);
		gChunkOperations = std::move(previousOperations);
		gFSOperations = std::move(previousFilesystem);
	}

	void report(uint32_t version, bool todel) {
		chunk_server_has_chunk(server, kChunkId,
		                       common::combineVersionWithTodelFlag(version, todel), type);
	}

	struct Query {
		uint64_t generation{};
		std::vector<uint64_t> ids;
	};

	Query takeQuery() {
		auto packets = Access::takePackets(server);
		EXPECT_EQ(packets.size(), 1U);
		if (packets.size() != 1) { return {}; }
		verifyHeader(packets.front(), SAU_MATOCS_QUERY_CHUNKS);
		removeHeaderInPlace(packets.front());
		Query query;
		matocs::queryChunks::deserialize(packets.front(), query.generation, query.ids);
		EXPECT_NE(query.generation, 0U);
		return query;
	}

	uint64_t query() {
		Access::tick();
		const auto query = takeQuery();
		EXPECT_EQ(query.ids, std::vector<uint64_t>{kChunkId});
		return query.generation;
	}

	void answerQuery(const Query &query, const std::vector<uint64_t> &ids) {
		std::vector<ChunkWithVersionAndType> parts;
		for (auto id : ids) {
			parts.emplace_back(id, common::combineVersionWithTodelFlag(kVersion, true), type);
		}
		auto packet = cstoma::queryChunks::build(query.generation, ids, parts);
		removeHeaderInPlace(packet);
		Access::reply(server, packet);
	}

	void reply(uint64_t generation, const std::vector<ChunkWithVersionAndType> &parts) {
		auto packet =
		    cstoma::queryChunks::build(generation, std::vector<uint64_t>{kChunkId}, parts);
		removeHeaderInPlace(packet);
		Access::reply(server, packet);
	}

	ChunkWithVersionAndType found(uint32_t version, bool todel) const {
		return {kChunkId, common::combineVersionWithTodelFlag(version, todel), type};
	}

	void expectState(uint32_t version, bool busy, bool todel) {
		const auto state = Access::part(server, kChunkId, type);
		EXPECT_EQ(state.chunkVersion, version);
		EXPECT_EQ(state.partVersion, version);
		EXPECT_TRUE(state.valid);
		EXPECT_EQ(state.busy, busy);
		EXPECT_EQ(state.todel, todel);
	}

	void expectIdle(std::size_t pending = 0) {
		Access::tick();
		EXPECT_TRUE(Access::takePackets(server).empty());
		EXPECT_EQ(Access::pendingRefreshes(server), pending);
		EXPECT_FALSE(Access::disconnectRequested(server));
	}

	void startVersionChange() {
		// A lost/reannounced copy owes a version bump before its next write.
		const auto previous = Access::part(server, kChunkId, type);
		chunk_lost(server, kChunkId, type);
		report(previous.chunkVersion, previous.todel);
		uint32_t lockId = 0;
		uint8_t operation = 0;
		uint64_t chunkId = 0;
		ASSERT_EQ(chunk_multi_modify(kChunkId, &lockId, 1, false, &operation, &chunkId, 0),
		          SAUNAFS_STATUS_OK);
		ASSERT_EQ(chunkId, kChunkId);
		// Discard the actual version command, not any refresh query.
		auto packets = Access::takePackets(server);
		ASSERT_EQ(packets.size(), 1U);
		verifyHeader(packets.front(), SAU_MATOCS_SET_VERSION);
	}

	void backlog(std::size_t count, uint64_t first = 100, matocsserventry *peer = nullptr) {
		if (peer == nullptr) { peer = server; }
		for (uint64_t id = first; id < first + count; ++id) {
			if (!chunk_exists(id)) {
				chunk_create_with_goal_counters(id, kVersion, {{1, 1}}, 0, 0);
			}
			chunk_server_has_chunk(peer, id, kVersion, type);
			chunk_server_has_chunk(peer, id,
			                       common::combineVersionWithTodelFlag(kVersion - 1, true), type);
		}
	}

	std::size_t queriedIds(matocsserventry *peer) {
		std::set<uint64_t> ids;
		for (auto &packet : Access::takePackets(peer)) {
			EXPECT_LE(packet.size(), kMaxMasterToChunkserverPacketSize);
			verifyHeader(packet, SAU_MATOCS_QUERY_CHUNKS);
			removeHeaderInPlace(packet);
			uint64_t generation = 0;
			std::vector<uint64_t> group;
			matocs::queryChunks::deserialize(packet, generation, group);
			for (auto id : group) { EXPECT_TRUE(ids.insert(id).second); }
		}
		return ids.size();
	}

	std::size_t answerQueries(matocsserventry *peer, std::set<uint64_t> &seen) {
		std::size_t count = 0;
		for (auto &packet : Access::takePackets(peer)) {
			verifyHeader(packet, SAU_MATOCS_QUERY_CHUNKS);
			removeHeaderInPlace(packet);
			uint64_t generation = 0;
			std::vector<uint64_t> ids;
			matocs::queryChunks::deserialize(packet, generation, ids);
			std::vector<ChunkWithVersionAndType> foundParts;
			for (auto id : ids) {
				EXPECT_TRUE(seen.insert(id).second) << id;
				foundParts.emplace_back(id, common::combineVersionWithTodelFlag(kVersion, true),
				                        type);
			}
			count += ids.size();
			auto response = cstoma::queryChunks::build(generation, ids, foundParts);
			removeHeaderInPlace(response);
			Access::reply(peer, response);
		}
		return count;
	}

	csdbentry entry;
	std::array<csdbentry, 7> extraEntries;
	std::array<csdbentry *, 7> previousExtraEntries{};
	csdbentry *previousEntry = nullptr;
	matocsserventry *server = nullptr;
	std::unique_ptr<IFilesystemOperations> previousFilesystem;
	std::unique_ptr<IChunkOperations> previousOperations;
	Signal<uint64_t, uint32_t, uint32_t, uint32_t> previousSignal;
};

}  // namespace

TEST_F(ChunkDiskStateTest, ClearsToDeleteUsingFreshStateWithoutRollingBackVersion) {
	report(kVersion, true);
	report(kVersion - 1, false);
	expectState(kVersion, false, true);

	reply(query(), {found(kVersion, false)});
	expectState(kVersion, false, false);
	expectIdle();
}

TEST_F(ChunkDiskStateTest, ObsoleteToDeleteFlagDoesNotOverrideFreshState) {
	// The disk was marked for deletion, then restored before the refresh ran.
	report(kVersion - 1, true);
	reply(query(), {found(kVersion, false)});
	expectState(kVersion, false, false);
	expectIdle();
}

TEST_F(ChunkDiskStateTest, RefreshPreservesBusyUntilVersionStatusArrives) {
	startVersionChange();
	report(kVersion, true);
	expectState(kVersion + 1, true, false);

	reply(query(), {found(kVersion + 1, true)});
	expectState(kVersion + 1, true, true);
	chunk_got_setversion_status(server, kChunkId, type, SAUNAFS_STATUS_OK);
	expectState(kVersion + 1, false, true);
	expectIdle();
}

TEST_F(ChunkDiskStateTest, ClearingToDeleteAlsoPreservesBusy) {
	report(kVersion, true);
	startVersionChange();
	report(kVersion, false);
	reply(query(), {found(kVersion + 1, false)});
	expectState(kVersion + 1, true, false);
	chunk_got_setversion_status(server, kChunkId, type, SAUNAFS_STATUS_OK);
	expectState(kVersion + 1, false, false);
	expectIdle();
}

TEST_F(ChunkDiskStateTest, LockedPartRetriesAfterEmptyReplyWithoutInvalidatingCopy) {
	startVersionChange();
	report(kVersion, true);
	const auto first = query();
	reply(first, {});
	expectState(kVersion + 1, true, false);
	expectIdle(1);
	Access::advanceTime(1000);

	const auto second = query();
	EXPECT_NE(first, second);
	reply(second, {});
	expectState(kVersion + 1, true, false);

	chunk_got_setversion_status(server, kChunkId, type, SAUNAFS_STATUS_OK);
	Access::advanceTime(1000);
	reply(query(), {found(kVersion + 1, true)});
	expectState(kVersion + 1, false, true);
	expectIdle();
}

TEST_F(ChunkDiskStateTest, StalePositiveReplyRetriesWithoutReplacingOperationVersion) {
	startVersionChange();
	report(kVersion, true);
	reply(query(), {found(kVersion, true)});
	expectState(kVersion + 1, true, false);

	chunk_got_setversion_status(server, kChunkId, type, SAUNAFS_STATUS_OK);
	reply(query(), {found(kVersion + 1, true)});
	expectState(kVersion + 1, false, true);
	expectIdle();
}

TEST_F(ChunkDiskStateTest, RepeatedStaleReportsCoalesceWhileQueryIsOutstanding) {
	for (unsigned i = 0; i < 100; ++i) { report(kVersion - 1, true); }
	const auto generation = query();
	for (unsigned i = 0; i < 100; ++i) {
		report(kVersion - 1, true);
		expectIdle(1);
	}

	reply(generation, {found(kVersion, true)});
	expectState(kVersion, false, true);
	expectIdle();
}

TEST_F(ChunkDiskStateTest, UnrelatedGenerationCannotAcknowledgeRefresh) {
	report(kVersion - 1, true);
	const auto generation = query();
	// A client-location reply can carry the same chunk ID, but has its own nonce.
	reply(generation + 1, {});
	expectIdle(1);
	reply(generation, {});
	Access::advanceTime(1000);
	const auto retry = query();
	EXPECT_NE(retry, generation);
	reply(retry, {found(kVersion, true)});
	expectState(kVersion, false, true);
	expectIdle();
}

TEST_F(ChunkDiskStateTest, PartialEcResponseKeepsMissingTypePending) {
	const auto otherType = slice_traits::ec::ChunkPartType(2, 1, 0);
	chunk_server_has_chunk(server, kChunkId, kVersion, otherType);
	report(kVersion - 1, true);
	chunk_server_has_chunk(server, kChunkId,
	                       common::combineVersionWithTodelFlag(kVersion - 1, true), otherType);

	reply(query(), {found(kVersion, true)});
	const auto pending = Access::part(server, kChunkId, otherType);
	EXPECT_TRUE(pending.valid);
	EXPECT_FALSE(pending.todel);
	EXPECT_EQ(pending.partVersion, kVersion);

	Access::advanceTime(1000);
	reply(query(), {{kChunkId, common::combineVersionWithTodelFlag(kVersion, true), otherType}});
	EXPECT_TRUE(Access::part(server, kChunkId, otherType).todel);
	expectState(kVersion, false, true);
	expectIdle();
}

TEST_F(ChunkDiskStateTest, DamageCancelsRefreshWithoutResurrectingInvalidCopy) {
	report(kVersion - 1, true);
	const auto generation = query();
	chunk_damaged(server, kChunkId, type);
	expectIdle();
	reply(generation, {found(kVersion, true)});
	EXPECT_FALSE(Access::part(server, kChunkId, type).valid);
	expectIdle();
}

TEST_F(ChunkDiskStateTest, LossCancelsRefreshInsteadOfRetryingRemovedCopy) {
	report(kVersion - 1, true);
	query();
	chunk_lost(server, kChunkId, type);
	EXPECT_FALSE(chunk_server_holds_valid_part(kChunkId, server, type));
	ChunkPartState state;
	EXPECT_FALSE(chunk_get_part_state(server, kChunkId, type, state));
	expectIdle();
}

TEST_F(ChunkDiskStateTest, SnapshotRejectsUnknownChunkOrPart) {
	ChunkPartState state;
	EXPECT_FALSE(chunk_get_part_state(server, kChunkId + 1, type, state));
	EXPECT_FALSE(
	    chunk_get_part_state(server, kChunkId, slice_traits::ec::ChunkPartType(2, 1, 0), state));
}

TEST_F(ChunkDiskStateTest, UnansweredRefreshDisconnectsInsteadOfQueueingRetries) {
	report(kVersion - 1, true);
	query();
	Access::expire(server, kChunkId);
	Access::tick();
	EXPECT_TRUE(Access::disconnectRequested(server));
	EXPECT_TRUE(Access::takePackets(server).empty());
	expectState(kVersion, false, false);
}

TEST_F(ChunkDiskStateTest, ReconnectionDoesNotInheritPendingRefresh) {
	report(kVersion - 1, true);
	query();
	Access::disconnect(server);
	server = Access::connect(entry);
	expectState(kVersion, false, false);
	expectIdle();
}

TEST_F(ChunkDiskStateTest, OldPeerReconnectsInsteadOfReceivingUnsupportedQuery) {
	Access::disconnect(server);
	server = Access::connect(entry, kFirstVersionWithChunkLocationQuery - 1);
	report(kVersion - 1, true);

	EXPECT_TRUE(Access::disconnectRequested(server));
	Access::tick();
	EXPECT_TRUE(Access::takePackets(server).empty());
	EXPECT_EQ(Access::pendingRefreshes(server), 0U);
	expectState(kVersion, false, false);
}

TEST_F(ChunkDiskStateTest, LargeBacklogBoundsWorkPerInvocation) {
	// Count real registry checks, not elapsed milliseconds: this must also fail
	// on a fast machine when one callback drains the entire backlog.
	backlog(10'000);
	Access::tick();
	const auto work = Access::lastWork();
	EXPECT_LE(work, kDiskStateRefreshWorkLimit);
	EXPECT_GT(queriedIds(server), 0U);
	EXPECT_EQ(Access::pendingRefreshes(server), 10'000U);
}

TEST_F(ChunkDiskStateTest, SlowPeerCannotAccumulateUnboundedQueries) {
	backlog(10'000);
	std::size_t sent = 0;
	for (unsigned slice = 0; slice < 40; ++slice) {
		Access::tick();
		sent += queriedIds(server);
	}
	// No reply arrived. Even across many slices, unsent work must stay pending
	// instead of moving the entire inventory into the socket output queue.
	EXPECT_GT(sent, 0U);
	EXPECT_LE(sent, kDiskStateRefreshOutstandingLimit);
	EXPECT_EQ(Access::pendingRefreshes(server), 10'000U);
}

TEST_F(ChunkDiskStateTest, ExpiredTimeBudgetYieldsWithoutDroppingWork) {
	backlog(10'000);
	Access::tick(0);
	const auto work = Access::lastWork();
	EXPECT_EQ(work, 0U);
	EXPECT_EQ(queriedIds(server), 0U);
	EXPECT_EQ(Access::pendingRefreshes(server), 10'000U);

	Access::tick();
	EXPECT_GT(queriedIds(server), 0U);
}

TEST_F(ChunkDiskStateTest, BacklogResumesAcrossRepliesInsertionsRemovalsAndRehash) {
	backlog(2'000);
	std::set<uint64_t> seen;
	Access::tick();
	ASSERT_GT(answerQueries(server, seen), 0U);

	// Add new keys and remove some old copies between slices. A retained map
	// iterator would be invalid after rehash; stable queued IDs must still work.
	backlog(2'000, 2'100);
	std::set<uint64_t> removed;
	for (uint64_t id = 100; id < 2'100 && removed.size() < 100; ++id) {
		if (!seen.contains(id)) {
			chunk_lost(server, id, type);
			removed.insert(id);
		}
	}
	Access::rehash(server);

	for (unsigned slice = 0; slice < 500 && Access::pendingRefreshes(server) != 0; ++slice) {
		Access::tick();
		const auto work = Access::lastWork();
		ASSERT_LE(work, kDiskStateRefreshWorkLimit);
		answerQueries(server, seen);
	}
	EXPECT_EQ(Access::pendingRefreshes(server), 0U);
	EXPECT_EQ(seen.size(), 4'000U - removed.size());
	for (auto id : removed) { EXPECT_FALSE(seen.contains(id)); }
	for (auto id : seen) {
		const auto state = Access::part(server, id, type);
		EXPECT_EQ(state.partVersion, kVersion);
		EXPECT_TRUE(state.valid);
		EXPECT_TRUE(state.todel);
	}
	expectIdle();
}

TEST_F(ChunkDiskStateTest, PendingQueueUnlinksHeadMiddleTailAndCanBeReused) {
	backlog(7);
	Access::tick();
	const auto first = takeQuery();
	ASSERT_EQ(first.ids, (std::vector<uint64_t>{100, 101, 102, 103, 104, 105, 106}));

	// Replies need not follow queue order. Exercise each unlink position,
	// including the last remaining record, before reusing an empty queue.
	answerQuery(first, {103, 106, 100});
	EXPECT_EQ(Access::pendingRefreshes(server), 4U);
	Access::rehash(server);
	answerQuery(first, {105, 101, 104, 102});
	expectIdle();

	backlog(1);
	Access::tick();
	const auto reused = takeQuery();
	ASSERT_EQ(reused.ids, (std::vector<uint64_t>{100}));
	answerQuery(reused, reused.ids);
	expectIdle();
}

TEST_F(ChunkDiskStateTest, PendingQueuePreservesInsertionsWhileEarlierKeysRetry) {
	backlog(3);
	Access::tick();
	const auto first = takeQuery();
	ASSERT_EQ(first.ids, (std::vector<uint64_t>{100, 101, 102}));

	// Only the middle key needs a delayed retry; the other two stay in flight.
	auto missing = cstoma::queryChunks::build(
	    first.generation, std::vector<uint64_t>{101}, std::vector<ChunkWithVersionAndType>{});
	removeHeaderInPlace(missing);
	Access::reply(server, missing);
	backlog(2, 200);
	Access::rehash(server);
	Access::tick();
	const auto added = takeQuery();
	ASSERT_EQ(added.ids, (std::vector<uint64_t>{200, 201}));
	answerQuery(added, added.ids);

	Access::advanceTime(1000);
	Access::tick();
	const auto retry = takeQuery();
	ASSERT_EQ(retry.ids, (std::vector<uint64_t>{101}));
	answerQuery(retry, retry.ids);
	answerQuery(first, {102, 100});
	expectIdle();
}

TEST_F(ChunkDiskStateTest, RemovingLostPartsUnlinksPendingHeadMiddleAndTail) {
	backlog(5);
	for (auto id : {100, 102, 104}) { chunk_lost(server, id, type); }
	Access::rehash(server);
	Access::tick();
	const auto remaining = takeQuery();
	ASSERT_EQ(remaining.ids, (std::vector<uint64_t>{101, 103}));
	EXPECT_EQ(Access::pendingRefreshes(server), 2U);
	answerQuery(remaining, remaining.ids);
	expectIdle();
}

TEST_F(ChunkDiskStateTest, OutOfOrderRepliesPreserveEarliestOutstandingDeadline) {
	backlog(2);
	Access::tick();
	const auto earlier = takeQuery();
	ASSERT_EQ(earlier.ids, (std::vector<uint64_t>{100, 101}));
	Access::advanceTime(1000);
	backlog(2, 200);
	Access::tick();
	const auto later = takeQuery();
	ASSERT_EQ(later.ids, (std::vector<uint64_t>{200, 201}));

	// Swapping the last outstanding ID into an erased slot would put the
	// later deadline first and hide the expired earlier request.
	answerQuery(earlier, {100});
	answerQuery(later, {200});
	Access::advanceTime(59'000);
	Access::tick();
	EXPECT_TRUE(Access::disconnectRequested(server));
	EXPECT_TRUE(Access::takePackets(server).empty());
	EXPECT_EQ(Access::pendingRefreshes(server), 2U);
}

TEST_F(ChunkDiskStateTest, BusyPeerDoesNotStarveOtherChunkservers) {
	backlog(10'000);
	auto *second = Access::connect(extraEntries[0]);
	auto *third = Access::connect(extraEntries[1]);
	backlog(10'000, 100, second);
	backlog(10'000, 100, third);

	std::array<std::size_t, 3> sent{};
	for (unsigned slice = 0; slice < 40; ++slice) {
		Access::tick();
		const auto work = Access::lastWork();
		ASSERT_LE(work, kDiskStateRefreshWorkLimit);
		sent[0] += queriedIds(server);
		sent[1] += queriedIds(second);
		sent[2] += queriedIds(third);
	}
	for (auto count : sent) {
		EXPECT_GT(count, 0U);
		EXPECT_LE(count, kDiskStateRefreshOutstandingLimit);
	}

	// Disconnect a server with a continuation. Other peers' deadlines must not
	// wait for their thousands of unsent records to be visited first.
	Access::disconnect(server);
	server = nullptr;
	Access::advanceTime(60'000);
	Access::tick();
	EXPECT_TRUE(Access::disconnectRequested(second));
	EXPECT_TRUE(Access::disconnectRequested(third));
	EXPECT_TRUE(Access::takePackets(second).empty());
	EXPECT_TRUE(Access::takePackets(third).empty());
}

TEST_F(ChunkDiskStateTest, PeerRotationResumesAfterGlobalBudgetIsExhausted) {
	std::vector<matocsserventry *> peers{server};
	for (auto &extra : extraEntries) { peers.push_back(Access::connect(extra)); }
	for (auto *peer : peers) { backlog(512, 100, peer); }
	std::vector<std::size_t> sent(peers.size());

	for (unsigned slice = 0; slice < 4; ++slice) {
		Access::tick();
		const auto work = Access::lastWork();
		EXPECT_LE(work, kDiskStateRefreshWorkLimit);
		for (std::size_t index = 0; index < peers.size(); ++index) {
			sent[index] += queriedIds(peers[index]);
		}
		if (slice == 0) {
			// More peers than fit in one callback: continuation is required.
			EXPECT_EQ(sent.back(), 0U);
		}
	}
	for (auto count : sent) { EXPECT_GT(count, 0U); }
}

TEST_F(ChunkDiskStateTest, BlockedPeerDoesNotPreventHealthyPeerFromDrainingBacklog) {
	backlog(10'000);
	auto *healthy = Access::connect(extraEntries[0]);
	backlog(512, 100, healthy);
	std::set<uint64_t> seen;
	std::size_t blockedSent = 0;

	for (unsigned slice = 0; slice < 100 && Access::pendingRefreshes(healthy) != 0; ++slice) {
		Access::tick();
		const auto work = Access::lastWork();
		EXPECT_LE(work, kDiskStateRefreshWorkLimit);
		blockedSent += queriedIds(server);
		answerQueries(healthy, seen);
	}
	EXPECT_EQ(seen.size(), 512U);
	EXPECT_EQ(Access::pendingRefreshes(healthy), 0U);
	EXPECT_EQ(Access::pendingRefreshes(server), 10'000U);
	EXPECT_LE(blockedSent, kDiskStateRefreshOutstandingLimit);
}

TEST_F(ChunkDiskStateTest, CongestedOutputPausesQueriesWithoutDiscardingRefreshes) {
	backlog(10'000);
	Access::fillOutput(server);
	for (unsigned slice = 0; slice < 40; ++slice) { Access::tick(); }
	auto packets = Access::takePackets(server);
	ASSERT_EQ(packets.size(), kDiskStateRefreshOutputLimit);
	for (const auto &packet : packets) { verifyHeader(packet, ANTOAN_NOP); }
	EXPECT_EQ(Access::pendingRefreshes(server), 10'000U);

	Access::tick();
	EXPECT_GT(queriedIds(server), 0U);
}

TEST_F(ChunkDiskStateTest, UndrainedRefreshOutputStaysWithinPacketAndByteBounds) {
	backlog(10'000);
	for (unsigned slice = 0; slice < 40; ++slice) { Access::tick(); }
	auto packets = Access::takePackets(server);
	ASSERT_FALSE(packets.empty());
	EXPECT_LE(packets.size(), kDiskStateRefreshOutputLimit);
	std::size_t bytes = 0;
	for (const auto &packet : packets) {
		verifyHeader(packet, SAU_MATOCS_QUERY_CHUNKS);
		EXPECT_LE(packet.size(), kMaxMasterToChunkserverPacketSize);
		bytes += packet.size();
	}
	EXPECT_LE(bytes, kDiskStateRefreshOutputLimit * kMaxMasterToChunkserverPacketSize);
	EXPECT_EQ(Access::pendingRefreshes(server), 10'000U);
}

TEST_F(ChunkDiskStateTest, ManyPartTypesResumeValidationWithinTheSameWorkBound) {
	// One key must not bypass the bound by holding hundreds of pending types.
	for (std::size_t index = 0; index < kDiskStateRefreshWorkLimit * 2; ++index) {
		const auto partType = slice_traits::ec::ChunkPartType(2 + index / 32, 1 + index % 32, 0);
		chunk_server_has_chunk(server, kChunkId, kVersion, partType);
		matocsserv_refresh_chunk_disk_state(server, kChunkId, partType);
	}
	Access::tick();
	EXPECT_LE(Access::lastWork(), kDiskStateRefreshWorkLimit);
	EXPECT_EQ(queriedIds(server), 0U);
	std::size_t sent = 0;
	for (unsigned slice = 0; slice < 20; ++slice) {
		Access::tick();
		EXPECT_LE(Access::lastWork(), kDiskStateRefreshWorkLimit);
		sent += queriedIds(server);
	}
	EXPECT_EQ(sent, 1U);
}
