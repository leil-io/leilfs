/*
   Copyright 2026 Leil Storage

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
#include <memory>
#include <vector>

#include "chunkserver/bgjobs.h"
#include "chunkserver/master_connection.h"
#include "common/slice_traits.h"
#include "protocol/cstoma.h"

TEST(MasterConnectionTests, DropsReplyFromPreviousSocket) {
	MasterConn connection("localhost", "9420", "default", nullptr, nullptr);
	connection.setMode(ConnectionMode::CONNECTED);
	auto callback = MasterConn::sauJobFinished(&connection);
	auto *packet = new OutputPacket;
	cstoma::createChunk::serialize(packet->packet, 123, slice_traits::standard::ChunkPartType(),
	                               SAUNAFS_STATUS_OK);

	connection.setMode(ConnectionMode::KILL);
	connection.setMode(ConnectionMode::CONNECTED);
	callback(SAUNAFS_STATUS_OK, packet);
	EXPECT_TRUE(connection.isOutputQueueEmpty());
}

TEST(MasterConnectionTests, DropsLockReplyFromPreviousSocket) {
	MasterConn connection("localhost", "9420", "default", nullptr, nullptr);
	connection.setMode(ConnectionMode::CONNECTED);
	auto callback = MasterConn::sauJobFinishedAndLock(&connection, 123,
	                                                  slice_traits::standard::ChunkPartType());
	auto *packet = new OutputPacket;
	cstoma::createChunk::serialize(packet->packet, 123, slice_traits::standard::ChunkPartType(),
	                               SAUNAFS_STATUS_OK);

	connection.setMode(ConnectionMode::KILL);
	connection.setMode(ConnectionMode::CONNECTED);
	callback(SAUNAFS_ERROR_NOTDONE, packet);
	EXPECT_TRUE(connection.isOutputQueueEmpty());
}

TEST(MasterConnectionTests, DestroyedConnectionDisarmsPendingReplies) {
	// masterconn_term releases the connection first and the job pools second. Dropping the last
	// pool reference runs the destructor, which flushes any status still queued into the callback
	// that was owed the reply. That callback must not reach the connection that no longer exists.
	//
	// This is a sanitizer reproducer and it asserts nothing on a plain build: the fault it guards
	// is a read of freed memory, which only a sanitizer turns into a failure. Run it under the
	// asan preset to gate the behaviour; a green plain build says only that it did not crash.
	std::vector<int> descriptors;
	auto pool = std::make_shared<MasterJobPool>("term-test", 1, 10, 1, descriptors);
	auto connection = std::make_unique<MasterConn>("localhost", "9420", "default", pool, nullptr);
	connection->setMode(ConnectionMode::CONNECTED);

	auto callback = MasterConn::sauJobFinished(connection.get());
	auto *packet = new OutputPacket;
	cstoma::createChunk::serialize(packet->packet, 123, slice_traits::standard::ChunkPartType(),
	                               SAUNAFS_STATUS_OK);

	connection.reset();
	callback(SAUNAFS_STATUS_OK, packet);
}
