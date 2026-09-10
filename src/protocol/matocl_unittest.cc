/*
   Copyright 2013-2014 EditShare
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
#include "protocol/matocl.h"

#include <gtest/gtest.h>

#include "unittests/chunk_type_constants.h"
#include "unittests/inout_pair.h"
#include "unittests/packet.h"

TEST(MatoclCommunicationTests, FuseReadChunkData) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, messageId,    512, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId,      87,  0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 52,  0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, fileLength,   124, 0);
	SAUNAFS_DEFINE_INOUT_VECTOR_PAIR(ChunkTypeWithAddress, serverList) = {
		ChunkTypeWithAddress(NetworkAddress(0xC0A80001, 8080), standard, SAUNAFS_VERSHEX),
		ChunkTypeWithAddress(NetworkAddress(0xC0A80002, 8081), xor_p_of_6, SAUNAFS_VERSHEX),
		ChunkTypeWithAddress(NetworkAddress(0xC0A80003, 8082), xor_1_of_6, SAUNAFS_VERSHEX),
		ChunkTypeWithAddress(NetworkAddress(0xC0A80004, 8084), xor_5_of_7, SAUNAFS_VERSHEX),
	};

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocl::fuseReadChunk::serialize(buffer,
			messageIdIn, fileLengthIn, chunkIdIn, chunkVersionIn, serverListIn));

	verifyHeader(buffer, SAU_MATOCL_FUSE_READ_CHUNK);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, matocl::fuseReadChunk::kECChunks_ResponsePacketVersion);
	ASSERT_NO_THROW(deserializePacketDataNoHeader(buffer, messageIdOut));
	ASSERT_NO_THROW(matocl::fuseReadChunk::deserialize(buffer,
			fileLengthOut, chunkIdOut, chunkVersionOut, serverListOut));

	SAUNAFS_VERIFY_INOUT_PAIR(messageId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(fileLength);
	SAUNAFS_VERIFY_INOUT_PAIR(serverList);
}

TEST(MatoclCommunicationTests, FuseReadChunkStatus) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, messageId, 512, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t,  status,    10,  0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocl::fuseReadChunk::serialize(buffer, messageIdIn, statusIn));

	verifyHeader(buffer, SAU_MATOCL_FUSE_READ_CHUNK);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, matocl::fuseReadChunk::kStatusPacketVersion);
	ASSERT_NO_THROW(deserializePacketDataNoHeader(buffer, messageIdOut));
	ASSERT_NO_THROW(matocl::fuseReadChunk::deserialize(buffer, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(messageId);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
}

TEST(MatoclCommunicationTests, FuseWriteChunkData) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, messageId,    512, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId,      87,  0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 52,  0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, fileLength,   124, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, lockId,       225, 0);
	SAUNAFS_DEFINE_INOUT_VECTOR_PAIR(ChunkTypeWithAddress, serverList) = {
			ChunkTypeWithAddress(NetworkAddress(0xC0A80001, 8080), standard, SAUNAFS_VERSHEX),
			ChunkTypeWithAddress(NetworkAddress(0xC0A80002, 8081), xor_p_of_6, SAUNAFS_VERSHEX),
			ChunkTypeWithAddress(NetworkAddress(0xC0A80003, 8082), xor_1_of_6, SAUNAFS_VERSHEX),
			ChunkTypeWithAddress(NetworkAddress(0xC0A80004, 8084), xor_5_of_7, SAUNAFS_VERSHEX),
	};

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocl::fuseWriteChunk::serialize(buffer,
			messageIdIn, fileLengthIn, chunkIdIn, chunkVersionIn, lockIdIn, serverListIn));

	verifyHeader(buffer, SAU_MATOCL_FUSE_WRITE_CHUNK);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, matocl::fuseWriteChunk::kECChunks_ResponsePacketVersion);
	ASSERT_NO_THROW(deserializePacketDataNoHeader(buffer, messageIdOut));
	ASSERT_NO_THROW(matocl::fuseWriteChunk::deserialize(buffer,
			fileLengthOut, chunkIdOut, chunkVersionOut, lockIdOut, serverListOut));

	SAUNAFS_VERIFY_INOUT_PAIR(messageId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(fileLength);
	SAUNAFS_VERIFY_INOUT_PAIR(serverList);
}

TEST(MatoclCommunicationTests, FuseWriteChunkStatus) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, messageId, 512, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t,  status,    10,  0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocl::fuseWriteChunk::serialize(buffer, messageIdIn, statusIn));

	verifyHeader(buffer, SAU_MATOCL_FUSE_WRITE_CHUNK);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, matocl::fuseReadChunk::kStatusPacketVersion);
	ASSERT_NO_THROW(deserializePacketDataNoHeader(buffer, messageIdOut));
	ASSERT_NO_THROW(matocl::fuseWriteChunk::deserialize(buffer, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(messageId);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
}

TEST(MatoclCommunicationTests, FuseWriteChunkEnd) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, messageId, 512, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t,  status,    10,  0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocl::fuseWriteChunkEnd::serialize(buffer, messageIdIn, statusIn));

	verifyHeader(buffer, SAU_MATOCL_FUSE_WRITE_CHUNK_END);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(deserializePacketDataNoHeader(buffer, messageIdOut));
	ASSERT_NO_THROW(matocl::fuseWriteChunkEnd::deserialize(buffer, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(messageId);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
}

TEST(MatoclCommunicationTests, XorChunksHealth) {
	SAUNAFS_DEFINE_INOUT_PAIR(bool, regular, true, false);
	ChunksAvailabilityState availIn, availOut;
	ChunksReplicationState replIn, replOut;

	availIn.addChunk(0, ChunksAvailabilityState::kSafe);
	availIn.addChunk(1, ChunksAvailabilityState::kEndangered);
	availIn.addChunk(2, ChunksAvailabilityState::kEndangered);
	availIn.addChunk(3, ChunksAvailabilityState::kLost);
	availIn.addChunk(4, ChunksAvailabilityState::kSafe);

	replIn.addChunk(0, 0, 1);
	replIn.addChunk(2, 1, 0);
	replIn.addChunk(2, 2, 10);
	replIn.addChunk(3, 15, 5);
	replIn.addChunk(4, 12, 13);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocl::chunksHealth::serialize(buffer, regularIn, availIn, replIn));

	verifyHeader(buffer, SAU_MATOCL_CHUNKS_HEALTH);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(matocl::chunksHealth::deserialize(buffer, regularOut, availOut, replOut));

	SAUNAFS_VERIFY_INOUT_PAIR(regular);
	for (uint8_t goal = 0; goal <= GoalId::kMax; ++goal) {
		EXPECT_EQ(availIn.safeChunks(goal), availOut.safeChunks(goal)) << "goal " << (int)goal;
		EXPECT_EQ(availIn.endangeredChunks(goal), availOut.endangeredChunks(goal));
		EXPECT_EQ(availIn.lostChunks(goal), availOut.lostChunks(goal));

		for (uint32_t part = 0; part < ChunksReplicationState::kMaxPartsCount; ++part) {
			EXPECT_EQ(replIn.chunksToReplicate(goal, part), replOut.chunksToReplicate(goal, part));
			EXPECT_EQ(replIn.chunksToDelete(goal, part), replOut.chunksToDelete(goal, part));
		}
	}
}

TEST(MatoclCommunicationTests, FuseDeleteAcl) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, messageId, 123, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, SAUNAFS_ERROR_EPERM, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocl::fuseDeleteAcl::serialize(buffer, messageIdIn, statusIn));

	verifyHeader(buffer, SAU_MATOCL_FUSE_DELETE_ACL);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(matocl::fuseDeleteAcl::deserialize(buffer.data(), buffer.size(),
			messageIdOut, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(messageId);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
}

TEST(MatoclCommunicationTests, FuseGetAclStatus) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, messageId, 123, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, SAUNAFS_ERROR_EPERM, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocl::fuseGetAcl::serialize(buffer, messageIdIn, statusIn));

	verifyHeader(buffer, SAU_MATOCL_FUSE_GET_ACL);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(matocl::fuseGetAcl::deserialize(buffer.data(), buffer.size(),
			messageIdOut, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(messageId);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
}

TEST(MatoclCommunicationTests, FuseGetAclResponse) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, messageId, 123, 0);
	AccessControlList aclIn, aclOut;
	aclIn.setMode(0750);
	aclIn.setEntry(AccessControlList::kMask, 0, 5);
	aclIn.setEntry(AccessControlList::kNamedGroup, 123, 7);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocl::fuseGetAcl::serialize(buffer, messageIdIn, aclIn));

	verifyHeader(buffer, SAU_MATOCL_FUSE_GET_ACL);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(matocl::fuseGetAcl::deserialize(buffer.data(), buffer.size(),
			messageIdOut, aclOut));

	SAUNAFS_VERIFY_INOUT_PAIR(messageId);
	EXPECT_EQ(aclIn, aclOut);
}

TEST(MatoclCommunicationTests, FuseSetAcl) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, messageId, 123, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, SAUNAFS_ERROR_EPERM, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocl::fuseSetAcl::serialize(buffer, messageIdIn, statusIn));

	verifyHeader(buffer, SAU_MATOCL_FUSE_SET_ACL);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(matocl::fuseSetAcl::deserialize(buffer.data(), buffer.size(),
			messageIdOut, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(messageId);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
}
