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

#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "common/saunafs_version.h"
#include "errors/leil_error_codes.h"
#include "master/chunk_operations_in_memory.h"
#include "master/chunks.h"
#include "master/chunkserver_db.h"
#include "master/filesystem.h"
#include "master/matocsserv.h"
#include "protocol/cstoma.h"
#include "protocol/matocs.h"
#include "unittests/packet.h"

// Exercise the existing packet handlers while keeping the connection definition private.
void matocsserv_sau_register_host(matocsserventry *, const std::vector<uint8_t> &);
void matocsserv_sau_register_space(matocsserventry *, const std::vector<uint8_t> &);
void matocsserv_sau_register_chunks_end(matocsserventry *, const std::vector<uint8_t> &);

// A chunkserver drops the connection on any packet larger than
// kMaxMasterToChunkserverPacketSize, so the master must never build a
// SAU_MATOCS_QUERY_CHUNKS packet past it. The number of chunks waiting on a
// location is bounded only by ON_DEMAND_CHUNK_QUERY_LIMIT (10000 by default),
// and at eight bytes an id that is eight times the packet limit.
//
// The system tests cannot reach this: a batch only fills with chunks blocked
// between two event-loop iterations, and the loop flushes constantly, so
// filling one takes a cluster's worth of clients blocking at the same instant.
// Measured on a four-mount setup with 1600 concurrent readers, the largest
// batch observed was six ids. Hence the unit test.

namespace {

/// Size of the packet the master would actually put on the wire for \p ids.
size_t serializedQueryPacketSize(const std::vector<uint64_t> &ids) {
	MessageBuffer packet;
	matocs::queryChunks::serialize(packet, 1, ids);
	return packet.size();
}

std::vector<uint64_t> makeChunkIds(size_t count) {
	std::vector<uint64_t> ids(count);
	std::iota(ids.begin(), ids.end(), 1);
	return ids;
}

}  // namespace

TEST(MatocsservChunkQuerySplitTests, EmptyInputProducesNoPackets) {
	EXPECT_TRUE(matocsserv_split_chunk_query_ids({}).empty());
}

TEST(MatocsservChunkQuerySplitTests, EverySerializedPacketFitsTheReceiverLimit) {
	// 10000 is ON_DEMAND_CHUNK_QUERY_LIMIT's default, i.e. the largest batch
	// the master can accumulate; the rest bracket the boundary.
	for (const size_t idCount : {size_t{1}, size_t{100}, size_t{1247}, size_t{1248}, size_t{1249},
	                             size_t{5000}, size_t{10000}}) {
		const auto groups = matocsserv_split_chunk_query_ids(makeChunkIds(idCount));

		ASSERT_FALSE(groups.empty()) << "no packets produced for " << idCount << " ids";
		for (const auto &group : groups) {
			EXPECT_FALSE(group.empty()) << "empty packet produced for " << idCount << " ids";
			EXPECT_LE(serializedQueryPacketSize(group), kMaxMasterToChunkserverPacketSize)
			    << "packet over the chunkserver's limit for " << idCount << " ids: the "
			    << "connection would be dropped instead of answering the query";
		}
	}
}

TEST(MatocsservChunkQuerySplitTests, EveryIdIsSentExactlyOnceInOrder) {
	// Splitting must not lose a chunk: an id dropped here is a client left
	// waiting for a location nobody was ever asked about.
	const auto ids = makeChunkIds(10000);
	const auto groups = matocsserv_split_chunk_query_ids(ids);

	std::vector<uint64_t> rejoined;
	for (const auto &group : groups) {
		rejoined.insert(rejoined.end(), group.begin(), group.end());
	}

	EXPECT_EQ(rejoined, ids);
}

TEST(MatocsservChunkQuerySplitTests, PacksIdsRatherThanSendingThemOneByOne) {
	// Splitting per id would be correct but would turn one packet into ten
	// thousand. Anything close to the limit is fine; one id per packet is not.
	const auto groups = matocsserv_split_chunk_query_ids(makeChunkIds(10000));

	ASSERT_FALSE(groups.empty());
	EXPECT_GT(groups.front().size(), 1000U)
	    << "expected the packet budget to be used, got " << groups.front().size() << " ids";
}

TEST(MatocsservChunkRegistrationConfigTests, CapsRecordsInFlightPerChunkserver) {
	EXPECT_EQ(4U, matocsserv_limit_chunk_registration_window(1000, 4));
	EXPECT_EQ(100U, matocsserv_limit_chunk_registration_window(1000, 1000));
	EXPECT_EQ(1U, matocsserv_limit_chunk_registration_window(100000, 2));
}

namespace {

class InventoryOperations : public ChunkOperationsInMemory {
public:
	bool requestsChunkserverIdentity() const override { return identity; }
	bool identity{false};
};

class MatocsservInventoryTest : public testing::Test {
protected:
	void SetUp() override {
		previousOperations = std::move(gChunkOperations);
		gChunkOperations = std::make_unique<InventoryOperations>();
		previousClusterId = std::exchange(gClusterId, "inventory-test");
		ASSERT_FALSE(matocsserv_has_incomplete_chunk_registration());
	}

	void TearDown() override {
		for (std::size_t index = 0; index < connections.size(); ++index) { disconnect(index); }
		EXPECT_FALSE(matocsserv_has_incomplete_chunk_registration());
		gChunkOperations = std::move(previousOperations);
		gClusterId = std::move(previousClusterId);
	}

	matocsserventry *connect(uint32_t version = kFirstVersionWithPullChunkRegistration) {
		const auto port = static_cast<uint16_t>(21000 + connections.size());
		auto *server = matocsserv_test_connection(nullptr, kAddress, port, version);
		connections.push_back(server);
		auto packet = cstoma::registerHost::build(kAddress, port, 60000, version, gClusterId);
		removeHeaderInPlace(packet);
		matocsserv_sau_register_host(server, packet);
		return server;
	}

	void disconnect(std::size_t index) {
		if (connections[index] == nullptr) { return; }
		matocsserv_test_connection_free(std::exchange(connections[index], nullptr));
		csdb_remove_server(kAddress, static_cast<uint16_t>(21000 + index));
	}

	void space(matocsserventry *server) {
		auto packet = cstoma::registerSpace::build(0, 0, 0, 0, 0, 0);
		removeHeaderInPlace(packet);
		matocsserv_sau_register_space(server, packet);
	}

	void end(matocsserventry *server) {
		auto packet = cstoma::registerChunksEnd::build(0);
		removeHeaderInPlace(packet);
		matocsserv_sau_register_chunks_end(server, packet);
	}

	static constexpr uint32_t kAddress = 0x0a000001;
	std::unique_ptr<IChunkOperations> previousOperations;
	std::string previousClusterId;
	std::vector<matocsserventry *> connections;
};

}  // namespace

TEST_F(MatocsservInventoryTest, PullSpaceKeepsDiscoveryOpenUntilEnd) {
	auto *server = connect();
	ASSERT_FALSE(matocsserv_is_killed(server));
	ASSERT_TRUE(matocsserv_has_incomplete_chunk_registration());
	space(server);
	EXPECT_TRUE(matocsserv_has_incomplete_chunk_registration());
	end(server);
	EXPECT_FALSE(matocsserv_has_incomplete_chunk_registration());
	// Duplicate completion and later space updates must not decrement twice.
	end(server);
	space(server);
	EXPECT_FALSE(matocsserv_has_incomplete_chunk_registration());
}

TEST_F(MatocsservInventoryTest, LegacySpaceRemovesOnlyTheUnqueryableInventory) {
	auto *legacy = connect(saunafsVersion(5, 12, 0));
	auto *current = connect();
	ChunkMutationPlan plan;
	plan.chunkId = plan.targetChunkId = 17;
	plan.chunkVersion = 3;
	plan.targetVersion = 4;
	ChunkMutationDiscovery discovery;
	ASSERT_EQ(matocsserv_check_mutation_discovery(plan, discovery), SAUNAFS_ERROR_NOCHUNKSERVERS);
	discovery.complete = true;
	EXPECT_EQ(matocsserv_check_mutation_discovery(plan, discovery), SAUNAFS_ERROR_NOCHUNKSERVERS);

	space(legacy);
	ASSERT_TRUE(matocsserv_has_incomplete_chunk_registration());
	discovery.complete = true;
	EXPECT_EQ(matocsserv_check_mutation_discovery(plan, discovery), SAUNAFS_STATUS_OK);
	end(current);
	EXPECT_FALSE(matocsserv_has_incomplete_chunk_registration());
}

TEST_F(MatocsservInventoryTest, DisconnectClearsUnfinishedInventories) {
	connect(saunafsVersion(5, 12, 0));
	connect();
	disconnect(0);
	EXPECT_TRUE(matocsserv_has_incomplete_chunk_registration());
	disconnect(1);
	EXPECT_FALSE(matocsserv_has_incomplete_chunk_registration());
}

TEST_F(MatocsservInventoryTest, IdentityRegistrationDoesNotRequireAnInventory) {
	static_cast<InventoryOperations *>(gChunkOperations.get())->identity = true;
	auto *server = connect(kFirstVersionWithChunkserverIdentity);
	ASSERT_FALSE(matocsserv_is_killed(server));
	EXPECT_FALSE(matocsserv_has_incomplete_chunk_registration());
}

TEST_F(MatocsservInventoryTest, RejectedHostDoesNotOpenDiscovery) {
	auto *server = connect(kFirstECVersion - 1);
	EXPECT_TRUE(matocsserv_is_killed(server));
	EXPECT_FALSE(matocsserv_has_incomplete_chunk_registration());
}
