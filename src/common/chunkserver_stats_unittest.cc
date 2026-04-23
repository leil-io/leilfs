/*
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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
#include "common/chunkserver_stats.h"

#include <gtest/gtest.h>

TEST(ChunkserverStatsTests, ChunkserverStatsCounters) {
	ChunkserverStats stats;
	NetworkAddress server1(1111, 11);
	NetworkAddress server2(2222, 22);

	EXPECT_EQ(0u, stats.getStatisticsFor(server1).pendingReads());
	EXPECT_EQ(0u, stats.getStatisticsFor(server2).pendingReads());
	EXPECT_EQ(0u, stats.getStatisticsFor(server1).pendingWrites());
	EXPECT_EQ(0u, stats.getStatisticsFor(server2).pendingWrites());

	stats.registerReadOperation(server1);
	EXPECT_EQ(1u, stats.getStatisticsFor(server1).pendingReads());

	stats.registerReadOperation(server2);
	EXPECT_EQ(1u, stats.getStatisticsFor(server1).pendingReads());
	EXPECT_EQ(1u, stats.getStatisticsFor(server2).pendingReads());

	EXPECT_EQ(1u, stats.getStatisticsFor(server1).getOperationCount());

	stats.unregisterReadOperation(server1);
	EXPECT_EQ(0u, stats.getStatisticsFor(server1).pendingReads());
	EXPECT_EQ(1u, stats.getStatisticsFor(server2).pendingReads());

	stats.registerReadOperation(server1);
	stats.registerReadOperation(server1);
	EXPECT_EQ(2u, stats.getStatisticsFor(server1).pendingReads());
	EXPECT_EQ(0u, stats.getStatisticsFor(server1).pendingWrites());

	stats.registerWriteOperation(server1);
	stats.registerWriteOperation(server1);
	stats.registerWriteOperation(server1);
	EXPECT_EQ(2u, stats.getStatisticsFor(server1).pendingReads());
	EXPECT_EQ(3u, stats.getStatisticsFor(server1).pendingWrites());
	EXPECT_EQ(5u, stats.getStatisticsFor(server1).getOperationCount());

	stats.unregisterWriteOperation(server1);
	stats.unregisterWriteOperation(server1);
	EXPECT_EQ(2u, stats.getStatisticsFor(server1).pendingReads());
	EXPECT_EQ(1u, stats.getStatisticsFor(server1).pendingWrites());
	EXPECT_EQ(3u, stats.getStatisticsFor(server1).getOperationCount());

	stats.unregisterReadOperation(server2);
	EXPECT_EQ(0u, stats.getStatisticsFor(server2).getOperationCount());
}

TEST(ChunkserverStatsTests, ChunkserverStatsDefectTracking) {
	ChunkserverStats stats;
	NetworkAddress server1(1111, 11);
	EXPECT_EQ(stats.getStatisticsFor(server1).score(), 1.);
	stats.markDefective(server1);
	EXPECT_LT(stats.getStatisticsFor(server1).score(), 1.);
	stats.markWorking(server1);
	EXPECT_EQ(stats.getStatisticsFor(server1).score(), 1.);
}

TEST(ChunkserverStatsTests, ChunkserverStatsRoundTripTimeTracking) {
	ChunkserverStats stats;
	NetworkAddress server1(1111, 11);
	NetworkAddress server2(2222, 22);

	stats.setUseRoundTripTime(true);
	stats.updateRoundTripTime(server1, 200);
	stats.updateRoundTripTime(server2, 20);

	EXPECT_TRUE(stats.getStatisticsFor(server1).hasRoundTripTime());
	EXPECT_EQ(200u, stats.getStatisticsFor(server1).roundTripTime_ms());
	EXPECT_GT(stats.getStatisticsFor(server2).score(), stats.getStatisticsFor(server1).score());

	stats.updateRoundTripTime(server1, 40);
	EXPECT_LT(stats.getStatisticsFor(server1).roundTripTime_ms(), 200u);
	EXPECT_GT(stats.getStatisticsFor(server1).roundTripTime_ms(), 40u);
}

TEST(ChunkserverStatsTests, ChunkserverStatsRoundTripTimeDisabled) {
	ChunkserverStats stats;
	NetworkAddress server1(1111, 11);
	NetworkAddress server2(2222, 22);

	stats.updateRoundTripTime(server1, 200);
	stats.updateRoundTripTime(server2, 20);

	EXPECT_FALSE(stats.getStatisticsFor(server1).hasRoundTripTime());
	EXPECT_FALSE(stats.getStatisticsFor(server2).hasRoundTripTime());
	EXPECT_EQ(stats.getStatisticsFor(server1).score(), 1.);
	EXPECT_EQ(stats.getStatisticsFor(server2).score(), 1.);
}

TEST(ChunkserverStatsTests, ChunkserverStatsDefectPenaltyOutweighsLatency) {
	ChunkserverStats stats;
	NetworkAddress fastServer(1111, 11);
	NetworkAddress slowServer(2222, 22);

	stats.setUseRoundTripTime(true);
	stats.updateRoundTripTime(fastServer, 5);
	stats.updateRoundTripTime(slowServer, 500);
	stats.markDefective(fastServer);

	EXPECT_LT(stats.getStatisticsFor(fastServer).score(),
	          stats.getStatisticsFor(slowServer).score());
}

TEST(ChunkserverStatsTests, ChunkserverStatsProxy) {
	ChunkserverStats stats;
	NetworkAddress server1(1111, 11);

	{
		ChunkserverStatsProxy proxy(stats);
		stats.setUseRoundTripTime(true);

		proxy.registerReadOperation(server1);
		proxy.registerReadOperation(server1);
		EXPECT_EQ(2u, stats.getStatisticsFor(server1).pendingReads());

		proxy.unregisterReadOperation(server1);
		EXPECT_EQ(1u, stats.getStatisticsFor(server1).pendingReads());

		proxy.registerWriteOperation(server1);
		proxy.registerWriteOperation(server1);
		EXPECT_EQ(2u, stats.getStatisticsFor(server1).pendingWrites());

		proxy.unregisterWriteOperation(server1);
		EXPECT_EQ(1u, stats.getStatisticsFor(server1).pendingWrites());

		proxy.updateRoundTripTime(server1, 25);
		EXPECT_TRUE(stats.getStatisticsFor(server1).hasRoundTripTime());
	}
	EXPECT_EQ(0u, stats.getStatisticsFor(server1).pendingReads());
	EXPECT_EQ(0u, stats.getStatisticsFor(server1).pendingWrites());
}

TEST(ChunkserverStatsTests, AllPendingDefectiveRead) {
	ChunkserverStats stats;
	NetworkAddress server1(1111, 11);
	ChunkserverStatsProxy proxy(stats);

	proxy.registerReadOperation(server1);
	EXPECT_EQ(stats.getStatisticsFor(server1).score(), 1.);
	proxy.allPendingDefective();
	EXPECT_LT(stats.getStatisticsFor(server1).score(), 1.);
}

TEST(ChunkserverStatsTests, AllPendingDefectiveWrite) {
	ChunkserverStats stats;
	NetworkAddress server1(1111, 11);
	ChunkserverStatsProxy proxy(stats);

	proxy.registerWriteOperation(server1);
	EXPECT_EQ(stats.getStatisticsFor(server1).score(), 1.);
	proxy.allPendingDefective();
	EXPECT_LT(stats.getStatisticsFor(server1).score(), 1.);
}

TEST(ChunkserverStatsTests, AllPendingDefectiveLonger) {
	ChunkserverStats stats;
	NetworkAddress server1(1111, 11);
	NetworkAddress server2(2222, 22);
	ChunkserverStatsProxy proxy(stats);

	proxy.registerReadOperation(server1);
	proxy.registerReadOperation(server2);
	proxy.registerReadOperation(server2);
	proxy.unregisterReadOperation(server1);
	proxy.unregisterReadOperation(server2);
	proxy.allPendingDefective();
	EXPECT_EQ(stats.getStatisticsFor(server1).score(), 1.);
	EXPECT_LT(stats.getStatisticsFor(server2).score(), 1.);
}
