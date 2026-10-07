/*
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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
#include "common/chunks_availability_state.h"

#include <gtest/gtest.h>

TEST(ChunksAvailabilityStateTests, AddRemoveChunk) {
	ChunksAvailabilityState s;

	s.addChunk(0, ChunksAvailabilityState::kSafe);
	s.addChunk(0, ChunksAvailabilityState::kSafe);
	s.addChunk(2, ChunksAvailabilityState::kSafe);
	s.addChunk(10, ChunksAvailabilityState::kSafe);
	s.addChunk(10, ChunksAvailabilityState::kEndangered);
	s.addChunk(10, ChunksAvailabilityState::kEndangered);
	s.addChunk(11, ChunksAvailabilityState::kEndangered);
	s.addChunk(10, ChunksAvailabilityState::kLost);

	EXPECT_EQ(2U, s.safeChunks(0));
	EXPECT_EQ(0U, s.endangeredChunks(0));
	EXPECT_EQ(0U, s.lostChunks(0));

	EXPECT_EQ(1U, s.safeChunks(2));
	EXPECT_EQ(0U, s.endangeredChunks(2));
	EXPECT_EQ(0U, s.lostChunks(2));

	EXPECT_EQ(1U, s.safeChunks(10));
	EXPECT_EQ(2U, s.endangeredChunks(10));
	EXPECT_EQ(1U, s.lostChunks(10));

	EXPECT_EQ(0U, s.safeChunks(11));
	EXPECT_EQ(1U, s.endangeredChunks(11));
	EXPECT_EQ(0U, s.lostChunks(11));

	// Make endangered chunks safe
	s.removeChunk(10, ChunksAvailabilityState::kEndangered);
	s.addChunk(10, ChunksAvailabilityState::kSafe);
	s.removeChunk(10, ChunksAvailabilityState::kEndangered);
	s.addChunk(10, ChunksAvailabilityState::kSafe);
	s.removeChunk(11, ChunksAvailabilityState::kEndangered);
	s.addChunk(11, ChunksAvailabilityState::kSafe);

	EXPECT_EQ(2U, s.safeChunks(0));
	EXPECT_EQ(0U, s.endangeredChunks(0));
	EXPECT_EQ(0U, s.lostChunks(0));

	EXPECT_EQ(1U, s.safeChunks(2));
	EXPECT_EQ(0U, s.endangeredChunks(2));
	EXPECT_EQ(0U, s.lostChunks(2));

	EXPECT_EQ(3U, s.safeChunks(10));
	EXPECT_EQ(0U, s.endangeredChunks(10));
	EXPECT_EQ(1U, s.lostChunks(10));

	EXPECT_EQ(1U, s.safeChunks(11));
	EXPECT_EQ(0U, s.endangeredChunks(11));
	EXPECT_EQ(0U, s.lostChunks(11));

	// Remove some safe chunks
	s.removeChunk(0, ChunksAvailabilityState::kSafe);
	s.removeChunk(0, ChunksAvailabilityState::kSafe);
	s.removeChunk(2, ChunksAvailabilityState::kSafe);

	EXPECT_EQ(0U, s.safeChunks(0));
	EXPECT_EQ(0U, s.endangeredChunks(0));
	EXPECT_EQ(0U, s.lostChunks(0));

	EXPECT_EQ(0U, s.safeChunks(2));
	EXPECT_EQ(0U, s.endangeredChunks(2));
	EXPECT_EQ(0U, s.lostChunks(2));
}

TEST(ChunksReplicationStateTests, AddRemoveChunk) {
	ChunksReplicationState s;
	s.addChunk(0, 0, 8);
	s.addChunk(2, 0, 0);
	s.addChunk(2, 1, 0);
	s.addChunk(10, 0, 4);
	s.addChunk(10, 1, 4);

	EXPECT_EQ(1U, s.chunksToReplicate(0, 0));
	EXPECT_EQ(1U, s.chunksToDelete(0, 8));
	EXPECT_EQ(1U, s.chunksToReplicate(2, 0));
	EXPECT_EQ(1U, s.chunksToReplicate(2, 1));
	EXPECT_EQ(2U, s.chunksToDelete(2, 0));
	EXPECT_EQ(1U, s.chunksToReplicate(10, 1));
	EXPECT_EQ(1U, s.chunksToReplicate(10, 0));
	EXPECT_EQ(2U, s.chunksToDelete(10, 4));

	// Replicate missing chunks
	s.removeChunk(2, 1, 0);
	s.addChunk(2, 0, 0);
	s.removeChunk(10, 1, 4);
	s.addChunk(10, 0, 4);

	EXPECT_EQ(2U, s.chunksToReplicate(2, 0));
	EXPECT_EQ(0U, s.chunksToReplicate(2, 1));
	EXPECT_EQ(2U, s.chunksToDelete(2, 0));
	EXPECT_EQ(0U, s.chunksToReplicate(10, 1));
	EXPECT_EQ(2U, s.chunksToReplicate(10, 0));
	EXPECT_EQ(2U, s.chunksToDelete(10, 4));
}

TEST(ChunksReplicationStateTests, MaximumValues) {
	ChunksReplicationState s;
	s.addChunk(10, 1500, 2000);
	s.addChunk(10, 1501, 2001);
	EXPECT_EQ(2U, s.chunksToReplicate(10, ChunksReplicationState::kMaxPartsCount - 1));
	EXPECT_EQ(2U, s.chunksToDelete(10, ChunksReplicationState::kMaxPartsCount - 1));
}

TEST(ChunksAvailabilityStateTests, RejectsInvalidGoalsInEveryState) {
	for (const uint8_t goal : {uint8_t{41}, uint8_t{255}}) {
		for (size_t state = 0; state < ChunksAvailabilityState::kStateCount; ++state) {
			std::array<std::map<uint8_t, uint64_t>, ChunksAvailabilityState::kStateCount> maps;
			maps[state][goal] = 1;
			std::vector<uint8_t> buffer;
			serialize(buffer, maps);
			ChunksAvailabilityState decoded;
			EXPECT_THROW(deserialize(buffer, decoded), IncorrectDeserializationException);
		}
	}
}

TEST(ChunksReplicationStateTests, RejectsInvalidGoalsInBothMaps) {
	using PartCounts = std::array<uint64_t, ChunksReplicationState::kMaxPartsCount>;
	for (const uint8_t goal : {uint8_t{41}, uint8_t{255}}) {
		for (size_t state = 0; state < 2; ++state) {
			std::array<std::map<uint8_t, PartCounts>, 2> maps;
			maps[state][goal][1] = 1;
			std::vector<uint8_t> buffer;
			serialize(buffer, maps);
			ChunksReplicationState decoded;
			EXPECT_THROW(deserialize(buffer, decoded), IncorrectDeserializationException);
		}
	}
}

TEST(ChunksAvailabilityStateTests, BoundaryGoalsKeepMapEncoding) {
	std::array<std::map<uint8_t, uint64_t>, ChunksAvailabilityState::kStateCount> maps;
	ChunksAvailabilityState state;
	for (const uint8_t goal : {uint8_t{0}, GoalId::kMax}) {
		for (size_t index = 0; index < maps.size(); ++index) {
			maps[index][goal] = 1;
			state.addChunk(goal, static_cast<ChunksAvailabilityState::State>(index));
		}
	}
	std::vector<uint8_t> expected, encoded;
	serialize(expected, maps);
	serialize(encoded, state);
	EXPECT_EQ(expected, encoded);
	ChunksAvailabilityState decoded;
	ASSERT_NO_THROW(deserialize(expected, decoded));
	std::vector<uint8_t> roundTrip;
	serialize(roundTrip, decoded);
	EXPECT_EQ(expected, roundTrip);
}

TEST(ChunksReplicationStateTests, BoundaryGoalsKeepMapEncoding) {
	using PartCounts = std::array<uint64_t, ChunksReplicationState::kMaxPartsCount>;
	std::array<std::map<uint8_t, PartCounts>, 2> maps;
	ChunksReplicationState state;
	for (const uint8_t goal : {uint8_t{0}, GoalId::kMax}) {
		maps[0][goal][1] = 1;
		maps[1][goal][2] = 1;
		state.addChunk(goal, 1, 2);
	}
	std::vector<uint8_t> expected, encoded;
	serialize(expected, maps);
	serialize(encoded, state);
	EXPECT_EQ(expected, encoded);
	ChunksReplicationState decoded;
	ASSERT_NO_THROW(deserialize(expected, decoded));
	std::vector<uint8_t> roundTrip;
	serialize(roundTrip, decoded);
	EXPECT_EQ(expected, roundTrip);
}
