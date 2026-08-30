/*
   Copyright 2026      Leil Storage OÜ

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

#include "chunkserver/chunk_generation_fence.h"

#include <gtest/gtest.h>

namespace {

class ChunkGenerationFenceTest : public ::testing::Test {
protected:
	void SetUp() override { chunk_generation_fence::resetForTest(); }
	void TearDown() override { chunk_generation_fence::resetForTest(); }
};

TEST_F(ChunkGenerationFenceTest, FirstGenerationAdmitsAndStaleIsRefused) {
	// The core ordering: once generation 5 was seen for a chunk, anything below it was issued
	// by a round the cluster has moved past and must not execute.
	EXPECT_TRUE(chunk_generation_fence::admit(1, 5, "accept"));
	EXPECT_FALSE(chunk_generation_fence::admit(1, 4, "execute"));
	EXPECT_TRUE(chunk_generation_fence::admit(1, 6, "accept"));
	EXPECT_FALSE(chunk_generation_fence::admit(1, 5, "execute"));
}

TEST_F(ChunkGenerationFenceTest, EqualGenerationReplayIsAdmitted) {
	// One round retries its own command; refusing the retry would wedge the round it protects.
	EXPECT_TRUE(chunk_generation_fence::admit(1, 5, "accept"));
	EXPECT_TRUE(chunk_generation_fence::admit(1, 5, "execute"));
}

TEST_F(ChunkGenerationFenceTest, ZeroGenerationBypassesAndRecordsNothing) {
	// Generation zero marks a command no round issued. It executes regardless of the recorded
	// high water and never occupies an entry: with capacity zero it still passes while the
	// first real generation is refused for want of a slot.
	chunk_generation_fence::setCapacityForTest(0);
	EXPECT_TRUE(chunk_generation_fence::admit(7, 0, "accept"));
	EXPECT_FALSE(chunk_generation_fence::admit(7, 1, "accept"));
	chunk_generation_fence::resetForTest();
	EXPECT_TRUE(chunk_generation_fence::admit(7, 3, "accept"));
	EXPECT_TRUE(chunk_generation_fence::admit(7, 0, "execute"));
	EXPECT_FALSE(chunk_generation_fence::admit(7, 2, "execute"));
}

TEST_F(ChunkGenerationFenceTest, ChunksAreFencedIndependently) {
	EXPECT_TRUE(chunk_generation_fence::admit(1, 9, "accept"));
	EXPECT_TRUE(chunk_generation_fence::admit(2, 1, "accept"));
	EXPECT_FALSE(chunk_generation_fence::admit(1, 8, "execute"));
	EXPECT_TRUE(chunk_generation_fence::admit(2, 2, "execute"));
}

TEST_F(ChunkGenerationFenceTest, FullTableRefusesUnseenChunksButServesTrackedOnes) {
	// Fail closed at the bound: a generation that cannot be recorded cannot be ordered, so it
	// is refused rather than executed unordered. Chunks already tracked keep working.
	chunk_generation_fence::setCapacityForTest(1);
	EXPECT_TRUE(chunk_generation_fence::admit(1, 5, "accept"));
	EXPECT_FALSE(chunk_generation_fence::admit(2, 1, "accept"));
	EXPECT_TRUE(chunk_generation_fence::admit(1, 6, "execute"));
}

TEST_F(ChunkGenerationFenceTest, SupersededReportsAGenerationBelowTheHighWater) {
	// A grant bound at 5 is superseded once a newer round admitted 6 for the chunk; the equal
	// and higher generations are the grant's own round and are not superseded.
	EXPECT_TRUE(chunk_generation_fence::admit(1, 6, "accept"));
	EXPECT_TRUE(chunk_generation_fence::isSuperseded(1, 5));
	EXPECT_FALSE(chunk_generation_fence::isSuperseded(1, 6));
	EXPECT_FALSE(chunk_generation_fence::isSuperseded(1, 7));
}

TEST_F(ChunkGenerationFenceTest, SupersededIsFalseForAnUntrackedChunkAndForGenerationZero) {
	// No recorded high water means no newer round has fenced the chunk, so a write proceeds; and
	// generation zero is the unfenced legacy plane, never superseded.
	EXPECT_FALSE(chunk_generation_fence::isSuperseded(99, 5));
	EXPECT_TRUE(chunk_generation_fence::admit(1, 6, "accept"));
	EXPECT_FALSE(chunk_generation_fence::isSuperseded(1, 0));
}

TEST_F(ChunkGenerationFenceTest, SupersededRecordsNothing) {
	// The check is read only: querying a generation neither creates an entry nor moves the high
	// water, so a chunk it was asked about is still admissible from its first real generation.
	chunk_generation_fence::setCapacityForTest(1);
	EXPECT_FALSE(chunk_generation_fence::isSuperseded(5, 3));
	EXPECT_TRUE(chunk_generation_fence::admit(5, 1, "accept"));
	EXPECT_FALSE(chunk_generation_fence::isSuperseded(5, 1));
}

}  // namespace
