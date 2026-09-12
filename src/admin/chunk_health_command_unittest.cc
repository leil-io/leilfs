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

#include <gtest/gtest.h>

#include "admin/chunk_health_command.h"

namespace {

ChunkHealthFreshness measuredFreshness() {
	ChunkHealthFreshness freshness;
	freshness.generation = 41;
	freshness.scanStart = 1789126800;
	freshness.scanEnd = 1789126803;
	freshness.chunksScanned = 123456;
	freshness.chunksExcluded = 7;
	freshness.chunkserversDown = 2;
	return freshness;
}

}  // namespace

TEST(ChunksHealthCommandTests, RowBeforeAnyMeasurement) {
	EXPECT_EQ("MEA 0 0 0 0 0 0", ChunksHealthCommand::freshnessRow(ChunkHealthFreshness(), 1000));
}

TEST(ChunksHealthCommandTests, RowCarriesTheSixFields) {
	// Age is the server clock past the scan end, duration the scan end past its start.
	EXPECT_EQ("MEA 41 7 3 123456 7 2",
	          ChunksHealthCommand::freshnessRow(measuredFreshness(), 1789126810));
}

TEST(ChunksHealthCommandTests, RowNeverGoesNegative) {
	auto freshness = measuredFreshness();
	freshness.scanEnd = freshness.scanStart - 1;
	EXPECT_EQ("MEA 41 0 0 123456 7 2",
	          ChunksHealthCommand::freshnessRow(freshness, freshness.scanEnd - 5));
}

TEST(ChunksHealthCommandTests, SummaryBeforeAnyMeasurement) {
	EXPECT_EQ(
	    "Chunk health has not been measured yet, so the counts below are not a statement about"
	    " the installation.",
	    ChunksHealthCommand::freshnessSummary(ChunkHealthFreshness(), 1000));
}

TEST(ChunksHealthCommandTests, SummaryDatesTheMeasurement) {
	EXPECT_EQ(
	    "Measured 7s ago (scan 41 took 3s, 123456 chunks, 7 excluded, 2 chunkservers unreachable)",
	    ChunksHealthCommand::freshnessSummary(measuredFreshness(), 1789126810));
}
