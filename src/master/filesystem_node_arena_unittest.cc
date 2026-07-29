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

#include "master/filesystem_node_arena.h"

#include <gtest/gtest.h>
#include <utility>

#include "master/filesystem_node_types.h"
#include "master/hstring_memstorage.h"

class FSNodeArenaTest : public ::testing::Test {
protected:
	void SetUp() override { hstorage::Storage::reset(new hstorage::MemStorage()); }

	// Nodes handed to adopt() are owned by the arena afterwards; leak and double-free
	// violations in these tests are caught by the sanitizer jobs.
	static FSNode *makeNode(inode_t inode) {
		FSNode *node = FSNode::create(FSNodeType::kFile);
		node->id = inode;
		return node;
	}
};

TEST_F(FSNodeArenaTest, LookupUnknownReturnsNullopt) {
	FSNodeArena arena;
	EXPECT_FALSE(arena.lookup(1).has_value());
}

TEST_F(FSNodeArenaTest, AdoptPinsAndLookupReturnsSameInstance) {
	FSNodeArena arena;
	FSNode *node = makeNode(7);
	EXPECT_EQ(arena.adopt(7, node), node);
	auto pinned = arena.lookup(7);
	ASSERT_TRUE(pinned.has_value());
	EXPECT_EQ(*pinned, node);
}

TEST_F(FSNodeArenaTest, ChunkTableMetadataDefaultsAndRoundTrips) {
	FSNodeArena arena;
	EXPECT_EQ(arena.chunkTableMeta(7), nullptr);

	auto &metadata = arena.ensureChunkTableMeta(7);
	EXPECT_EQ(metadata.liveChunkCount, 0U);
	EXPECT_EQ(metadata.chunkTableSize, 0U);
	EXPECT_EQ(metadata.chunkTableRevision, 0U);

	metadata.liveChunkCount = 4;
	metadata.chunkTableSize = 8;
	const auto *stored = std::as_const(arena).chunkTableMeta(7);
	ASSERT_NE(stored, nullptr);
	EXPECT_EQ(stored->liveChunkCount, 4U);
	EXPECT_EQ(stored->chunkTableSize, 8U);
}

TEST_F(FSNodeArenaTest, AdoptDuplicateKeepsPinnedInstance) {
	FSNodeArena arena;
	FSNode *first = makeNode(7);
	ASSERT_EQ(arena.adopt(7, first), first);
	// The duplicate is destroyed by the arena; the pinned instance stays valid.
	FSNode *returned = arena.adopt(7, makeNode(7));
	EXPECT_EQ(returned, first);
	auto pinned = arena.lookup(7);
	ASSERT_TRUE(pinned.has_value());
	EXPECT_EQ(*pinned, first);
}

TEST_F(FSNodeArenaTest, ReleaseAndTombstoneMakesLookupReturnNull) {
	FSNodeArena arena;
	FSNode *node = makeNode(7);
	ASSERT_EQ(arena.adopt(7, node), node);
	arena.ensureChunkTableMeta(7).chunkTableSize = 8;
	arena.releaseAndTombstone(7);
	auto entry = arena.lookup(7);
	ASSERT_TRUE(entry.has_value());
	EXPECT_EQ(*entry, nullptr);
	EXPECT_EQ(arena.chunkTableMeta(7), nullptr);
	// The arena released ownership; the caller-side owner destroys the node
	// (removeNode in production).
	FSNode::destroy(node);
}

TEST_F(FSNodeArenaTest, ReleaseNodeOwnershipRetainsChunkTableMetadata) {
	FSNodeArena arena;
	FSNode *node = makeNode(7);
	ASSERT_EQ(arena.adopt(7, node), node);
	arena.ensureChunkTableMeta(7).chunkTableSize = 8;

	arena.releaseNodeOwnershipAndTombstone(7);

	auto entry = arena.lookup(7);
	ASSERT_TRUE(entry.has_value());
	EXPECT_EQ(*entry, nullptr);
	ASSERT_NE(arena.chunkTableMeta(7), nullptr);
	EXPECT_EQ(arena.chunkTableMeta(7)->chunkTableSize, 8U);

	arena.releaseAndTombstone(7);
	EXPECT_EQ(arena.chunkTableMeta(7), nullptr);
	FSNode::destroy(node);
}

TEST_F(FSNodeArenaTest, TombstoneWithoutAdoptIsVisible) {
	FSNodeArena arena;
	arena.releaseAndTombstone(7);
	auto entry = arena.lookup(7);
	ASSERT_TRUE(entry.has_value());
	EXPECT_EQ(*entry, nullptr);
}

TEST_F(FSNodeArenaTest, AdoptOverTombstoneRePins) {
	FSNodeArena arena;
	FSNode *first = makeNode(7);
	ASSERT_EQ(arena.adopt(7, first), first);
	arena.releaseAndTombstone(7);
	FSNode::destroy(first);

	FSNode *recreated = makeNode(7);
	EXPECT_EQ(arena.adopt(7, recreated), recreated);
	auto pinned = arena.lookup(7);
	ASSERT_TRUE(pinned.has_value());
	EXPECT_EQ(*pinned, recreated);
}

TEST_F(FSNodeArenaTest, MoveConstructorTransfersOwnershipAndEmptiesSource) {
	FSNodeArena source;
	FSNode *node = makeNode(7);
	ASSERT_EQ(source.adopt(7, node), node);
	source.ensureChunkTableMeta(7).chunkTableSize = 8;

	FSNodeArena destination(std::move(source));
	auto pinned = destination.lookup(7);
	ASSERT_TRUE(pinned.has_value());
	EXPECT_EQ(*pinned, node);
	ASSERT_NE(destination.chunkTableMeta(7), nullptr);
	EXPECT_EQ(destination.chunkTableMeta(7)->chunkTableSize, 8U);
	// The moved-from arena must be empty, or its destructor would double-free.
	EXPECT_FALSE(source.lookup(7).has_value());
	EXPECT_EQ(source.chunkTableMeta(7), nullptr);
}

TEST_F(FSNodeArenaTest, MoveAssignmentReplacesOwnedNodesAndEmptiesSource) {
	FSNodeArena source;
	FSNode *kept = makeNode(7);
	ASSERT_EQ(source.adopt(7, kept), kept);
	source.ensureChunkTableMeta(7).liveChunkCount = 4;

	FSNodeArena destination;
	// The destination's prior node is destroyed by the assignment.
	ASSERT_NE(destination.adopt(9, makeNode(9)), nullptr);
	destination.ensureChunkTableMeta(9).liveChunkCount = 2;

	destination = std::move(source);
	auto pinned = destination.lookup(7);
	ASSERT_TRUE(pinned.has_value());
	EXPECT_EQ(*pinned, kept);
	ASSERT_NE(destination.chunkTableMeta(7), nullptr);
	EXPECT_EQ(destination.chunkTableMeta(7)->liveChunkCount, 4U);
	EXPECT_FALSE(destination.lookup(9).has_value());
	EXPECT_EQ(destination.chunkTableMeta(9), nullptr);
	EXPECT_FALSE(source.lookup(7).has_value());
	EXPECT_EQ(source.chunkTableMeta(7), nullptr);
}
