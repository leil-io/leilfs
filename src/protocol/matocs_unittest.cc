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
#include "protocol/matocs.h"

#include <gtest/gtest.h>

#include "common/datapack.h"
#include "common/metadata_cluster_member.h"
#include "unittests/chunk_type_constants.h"
#include "unittests/inout_pair.h"
#include "unittests/packet.h"

TEST(MatocsCommunicationTests, ClusterMembers) {
	const std::vector<MetadataClusterMember> expected = {
	    {2, 0x7f000001, 9422, kFirstVersionWithChunkserverIdentity}};
	std::vector<uint8_t> buffer;
	matocs::clusterMembers::serialize(buffer, 1U, expected);
	verifyHeader(buffer, SAU_MATOCS_CLUSTER_MEMBERS);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, matocs::clusterMembers::kDefault);
	uint32_t seedId = 0;
	std::vector<MetadataClusterMember> decoded;
	matocs::clusterMembers::deserialize(buffer, seedId, decoded);
	EXPECT_EQ(seedId, 1U);
	ASSERT_EQ(decoded.size(), 1U);
	EXPECT_EQ(decoded.front().serverId, expected.front().serverId);
	EXPECT_EQ(decoded.front().ip, expected.front().ip);
	EXPECT_EQ(decoded.front().port, expected.front().port);
	EXPECT_EQ(decoded.front().version, expected.front().version);
	EXPECT_TRUE(isValidMetadataClusterSnapshot(seedId, decoded));
}

TEST(MatocsCommunicationTests, RejectsInvalidClusterMembers) {
	const MetadataClusterMember valid{2, 0x7f000001, 9422, kFirstVersionWithChunkserverIdentity};
	EXPECT_TRUE(isValidMetadataClusterSnapshot(1, {}));
	EXPECT_FALSE(isValidMetadataClusterSnapshot(0, {}));
	EXPECT_FALSE(isValidMetadataClusterSnapshot(2, {valid}));
	EXPECT_FALSE(isValidMetadataClusterSnapshot(1, {valid, valid}));
	for (int field = 0; field < 4; ++field) {
		auto invalid = valid;
		if (field == 0) { invalid.serverId = 0; }
		if (field == 1) { invalid.ip = 0; }
		if (field == 2) { invalid.port = 0; }
		if (field == 3) { invalid.version = kFirstVersionWithChunkserverIdentity - 1; }
		EXPECT_FALSE(isValidMetadataClusterSnapshot(1, {invalid}));
	}
	auto sameEndpoint = valid;
	sameEndpoint.serverId = 3;
	EXPECT_FALSE(isValidMetadataClusterSnapshot(1, {valid, sameEndpoint}));
	auto sameIdentity = valid;
	sameIdentity.port = 9423;
	EXPECT_FALSE(isValidMetadataClusterSnapshot(1, {valid, sameIdentity}));
	std::vector<MetadataClusterMember> excessive;
	for (uint32_t index = 1; index < kMaxMetadataConnections; ++index) {
		excessive.emplace_back(index + 1, valid.ip, valid.port + index, valid.version);
	}
	EXPECT_TRUE(isValidMetadataClusterSnapshot(1, excessive));
	excessive.emplace_back(kMaxMetadataConnections + 1, valid.ip, valid.port, valid.version);
	EXPECT_FALSE(isValidMetadataClusterSnapshot(1, excessive));
}

TEST(MatocsCommunicationTests, ClusterSnapshotRejectsMalformedPackets) {
	const std::vector<MetadataClusterMember> members = {
	    {2, 0x7f000001, 9422, kFirstVersionWithChunkserverIdentity}};
	auto packet = matocs::clusterMembers::build(1U, members);
	removeHeaderInPlace(packet);
	EXPECT_NO_THROW(matocs::clusterMembers::readSnapshot(packet));
	for (size_t length = 0; length < packet.size(); ++length) {
		const std::vector<uint8_t> truncated(packet.begin(), packet.begin() + length);
		EXPECT_THROW(matocs::clusterMembers::readSnapshot(truncated),
		             IncorrectDeserializationException);
	}
	auto invalid = packet;
	invalid.push_back(0);
	EXPECT_THROW(matocs::clusterMembers::readSnapshot(invalid), IncorrectDeserializationException);
	invalid = packet;
	uint8_t *field = invalid.data();
	put32bit(&field, 1);
	EXPECT_THROW(matocs::clusterMembers::readSnapshot(invalid), IncorrectDeserializationException);
	invalid = packet;
	field = invalid.data() + 8;
	put32bit(&field, UINT32_MAX);
	EXPECT_THROW(matocs::clusterMembers::readSnapshot(invalid), IncorrectDeserializationException);
}

TEST(MatocsCommunicationTests, RequestChunkserverId) {
	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocs::requestChunkserverId::serialize(buffer));

	verifyHeader(buffer, SAU_MATOCS_REQUEST_CHUNKSERVER_ID);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, matocs::requestChunkserverId::kDefault);
	ASSERT_NO_THROW(matocs::requestChunkserverId::deserialize(buffer));
}

TEST(MatocsCommunicationTests, RequestChunkserverIdRejectsWrongVersion) {
	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocs::requestChunkserverId::serialize(buffer));
	removeHeaderInPlace(buffer);

	uint8_t *version = buffer.data();
	put32bit(&version, matocs::requestChunkserverId::kDefault + 1);
	EXPECT_ANY_THROW(matocs::requestChunkserverId::deserialize(buffer));
}

TEST(MatocsCommunicationTests, SetVersion) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 87,  0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 52,  0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, newVersion, 53,  0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocs::setVersion::serialize(buffer,
			chunkIdIn, chunkTypeIn, chunkVersionIn, newVersionIn));

	verifyHeader(buffer, SAU_MATOCS_SET_VERSION);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, matocs::setVersion::kECChunks);
	ASSERT_NO_THROW(matocs::setVersion::deserialize(buffer,
			chunkIdOut, chunkTypeOut, chunkVersionOut, newVersionOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(newVersion);
}

TEST(MatocsCommunicationTests, ProbeChunk) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 87, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocs::probeChunk::serialize(buffer, chunkIdIn, chunkTypeIn));

	verifyHeader(buffer, SAU_MATOCS_PROBE_CHUNK);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, matocs::probeChunk::kDefault);
	ASSERT_NO_THROW(matocs::probeChunk::deserialize(buffer, chunkIdOut, chunkTypeOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
}

TEST(MatocsCommunicationTests, SetVersionAndLock) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 87, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 52, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, newVersion, 53, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocs::setVersionAndLock::serialize(buffer, chunkIdIn, chunkTypeIn,
	                                                     chunkVersionIn, newVersionIn));

	verifyHeader(buffer, SAU_MATOCS_SET_VERSION_AND_LOCK);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, matocs::setVersionAndLock::kECChunks);
	ASSERT_NO_THROW(matocs::setVersionAndLock::deserialize(buffer, chunkIdOut, chunkTypeOut,
	                                                       chunkVersionOut, newVersionOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(newVersion);
}

TEST(MatocsCommunicationTests, ChunkLock) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 87, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocs::chunkLock::serialize(buffer, chunkIdIn, chunkTypeIn));

	verifyHeader(buffer, SAU_MATOCS_LOCK_CHUNK);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, matocs::chunkLock::kECChunks);
	ASSERT_NO_THROW(matocs::chunkLock::deserialize(buffer, chunkIdOut, chunkTypeOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
}

TEST(MatocsCommunicationTests, DeleteChunk) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 87,  0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 52,  0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocs::deleteChunk::serialize(buffer,
			chunkIdIn, chunkTypeIn, chunkVersionIn));

	verifyHeader(buffer, SAU_MATOCS_DELETE_CHUNK);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(matocs::deleteChunk::deserialize(buffer,
			chunkIdOut, chunkTypeOut, chunkVersionOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
}

TEST(MatocsCommunicationTests, Replicate) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 87,  0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 52,  0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_VECTOR_PAIR(ChunkTypeWithAddress, serverList) = {
		ChunkTypeWithAddress(NetworkAddress(0xC0A80001, 8080), standard, SAUNAFS_VERSHEX),
		ChunkTypeWithAddress(NetworkAddress(0xC0A80002, 8081), xor_p_of_6, SAUNAFS_VERSHEX),
		ChunkTypeWithAddress(NetworkAddress(0xC0A80003, 8082), xor_1_of_6, SAUNAFS_VERSHEX),
		ChunkTypeWithAddress(NetworkAddress(0xC0A80004, 8084), xor_5_of_7, SAUNAFS_VERSHEX),
	};

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocs::replicateChunk::serialize(buffer,
			chunkIdIn, chunkVersionIn, chunkTypeIn, serverListIn));

	verifyHeader(buffer, SAU_MATOCS_REPLICATE_CHUNK);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(matocs::replicateChunk::deserialize(buffer,
			chunkIdOut, chunkVersionOut, chunkTypeOut, serverListOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(serverList);
}
