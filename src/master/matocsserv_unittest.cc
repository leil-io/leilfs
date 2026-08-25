/*
   Copyright 2026      Leil Storage OÜ

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

#include <cstdint>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

#include "master/matocsserv.h"
#include "protocol/matocs.h"

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
