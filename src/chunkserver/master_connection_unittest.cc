/*
   Copyright 2026 Leil Storage OÜ

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

#include <memory>
#include <utility>

#include <gtest/gtest.h>

#include "chunkserver/master_connection.h"

class MasterConnOutputQueueTests : public testing::Test {
protected:
	MasterConn masterConn_{"", "", "", std::shared_ptr<MasterJobPool>{},
	                       std::shared_ptr<MasterJobPool>{}};

	MessageBuffer popNextPacket() {
		auto &packets = masterConn_.nextOutputPacketQueue();
		auto packet = std::move(packets.front().packet);
		packets.pop();
		return packet;
	}

	void markOrdinaryHeadPartiallySent() { masterConn_.outputPackets_.front().bytesSent = 1; }
};

TEST_F(MasterConnOutputQueueTests, PriorityPacketsAreFifoAndOvertakeUnstartedPackets) {
	masterConn_.createAttachedPacket(MessageBuffer{0});
	masterConn_.createAttachedPriorityPacket(MessageBuffer{1});
	masterConn_.createAttachedPriorityPacket(MessageBuffer{2});

	EXPECT_EQ(MessageBuffer{1}, popNextPacket());
	EXPECT_EQ(MessageBuffer{2}, popNextPacket());
	EXPECT_EQ(MessageBuffer{0}, popNextPacket());
}

TEST_F(MasterConnOutputQueueTests, PartiallySentPacketPrecedesPriorityPackets) {
	masterConn_.createAttachedPacket(MessageBuffer{0, 0});
	markOrdinaryHeadPartiallySent();
	masterConn_.createAttachedPriorityPacket(MessageBuffer{1});
	masterConn_.createAttachedPriorityPacket(MessageBuffer{2});

	EXPECT_EQ((MessageBuffer{0, 0}), popNextPacket());
	EXPECT_EQ(MessageBuffer{1}, popNextPacket());
	EXPECT_EQ(MessageBuffer{2}, popNextPacket());
}
