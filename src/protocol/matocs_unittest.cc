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
#include "protocol/matocs.h"

#include <gtest/gtest.h>

#include "unittests/chunk_type_constants.h"
#include "unittests/inout_pair.h"
#include "unittests/packet.h"

TEST(MatocsCommunicationTests, ClusterMembers) {
	SAUNAFS_DEFINE_INOUT_VECTOR_PAIR(MetadataserverClusterEntry, members) = {
	    MetadataserverClusterEntry(1, 0xC0A80001, 9422, SAUNAFS_VERSHEX),
	    MetadataserverClusterEntry(2, 0xC0A80002, 9423, SAUNAFS_VERSHEX),
	    MetadataserverClusterEntry(3, 0xC0A80003, 9424, SAUNAFS_VERSHEX - 1),
	};

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocs::clusterMembers::serialize(buffer, membersIn));

	verifyHeader(buffer, SAU_MATOCS_CLUSTER_MEMBERS);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, 0);
	ASSERT_NO_THROW(matocs::clusterMembers::deserialize(buffer, membersOut));

	SAUNAFS_VERIFY_INOUT_PAIR(members);

	buffer.pop_back();
	std::vector<MetadataserverClusterEntry> truncatedMembers;
	ASSERT_THROW(matocs::clusterMembers::deserialize(buffer, truncatedMembers),
	             IncorrectDeserializationException);
}

TEST(MatocsCommunicationTests, RegisterPassive) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 0, 0xFF);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, version, SAUNAFS_VERSHEX, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(std::string, clusterId, "cluster-1", "");

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocs::registerPassive::serialize(buffer, statusIn, versionIn, clusterIdIn));

	verifyHeader(buffer, SAU_MATOCS_REGISTER_PASSIVE);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, 0);
	ASSERT_NO_THROW(
	    matocs::registerPassive::deserialize(buffer, statusOut, versionOut, clusterIdOut));

	SAUNAFS_VERIFY_INOUT_PAIR(status);
	SAUNAFS_VERIFY_INOUT_PAIR(version);
	SAUNAFS_VERIFY_INOUT_PAIR(clusterId);
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
