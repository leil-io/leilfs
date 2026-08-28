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

#include "protocol/matocs.h"
#include "common/platform.h"

#include <gtest/gtest.h>

#include "errors/saunafs_error_codes.h"
#include "unittests/chunk_type_constants.h"
#include "unittests/inout_pair.h"
#include "unittests/packet.h"

TEST(MatocsCommunicationTests, LegacyRegisterHostRoundTrip) {
	uint8_t status = SAUNAFS_STATUS_OK;
	uint32_t version = SAUNAFS_VERSHEX;
	std::string clusterId = "legacy-cluster";
	std::vector<uint8_t> buffer;
	matocs::registerHost::serialize(buffer, status, version, clusterId);

	verifyHeader(buffer, SAU_MATOCS_REGISTER_HOST);
	removeHeaderInPlace(buffer);
	uint8_t decodedStatus = SAUNAFS_ERROR_UNKNOWN;
	uint32_t decodedVersion = 0;
	std::string decodedClusterId;
	matocs::registerHost::deserialize(buffer, decodedStatus, decodedVersion, decodedClusterId);
	EXPECT_EQ(decodedStatus, status);
	EXPECT_EQ(decodedVersion, version);
	EXPECT_EQ(decodedClusterId, clusterId);
}

TEST(MatocsCommunicationTests, RegisterDistributedAndLeaseRoundTrip) {
	std::vector<uint8_t> registration;
	matocs::registerDistributed::serialize(registration, SAUNAFS_STATUS_OK, 7, 3,
	                                       0x1112131415161718ULL, SAUNAFS_VERSHEX,
	                                       std::string("distributed-cluster"), 5, 10'000, 40,
	                                       0x0102030405060708ULL);
	verifyHeader(registration, SAU_MATOCS_REGISTER_DISTRIBUTED);
	removeHeaderInPlace(registration);

	uint8_t status = SAUNAFS_ERROR_UNKNOWN;
	uint32_t stableId = 0;
	uint32_t mdsId = 0;
	uint64_t mdsIncarnation = 0;
	uint32_t version = 0;
	std::string clusterId;
	uint64_t sequence = 0;
	uint64_t deadline = 0;
	uint64_t cutoffReserve = 0;
	uint64_t claimToken = 0;
	matocs::registerDistributed::deserialize(registration, status, stableId, mdsId, mdsIncarnation,
	                                         version, clusterId, sequence, deadline, cutoffReserve,
	                                         claimToken);
	EXPECT_EQ(status, SAUNAFS_STATUS_OK);
	EXPECT_EQ(stableId, 7U);
	EXPECT_EQ(mdsId, 3U);
	EXPECT_EQ(mdsIncarnation, 0x1112131415161718ULL);
	EXPECT_EQ(version, static_cast<uint32_t>(SAUNAFS_VERSHEX));
	EXPECT_EQ(clusterId, "distributed-cluster");
	EXPECT_EQ(sequence, 5U);
	EXPECT_EQ(deadline, 10'000U);
	EXPECT_EQ(cutoffReserve, 40U);
	EXPECT_EQ(claimToken, 0x0102030405060708ULL);

	std::vector<uint8_t> lease;
	matocs::chunkserverSessionLease::serialize(lease, 7, 0x2122232425262728ULL, 3,
	                                           0x1112131415161718ULL, 6, 10'020, 40);
	verifyHeader(lease, SAU_MATOCS_CS_SESSION_LEASE);
	removeHeaderInPlace(lease);

	uint32_t leaseStableId = 0;
	uint64_t leaseIncarnation = 0;
	uint32_t renewerMdsId = 0;
	uint64_t renewerMdsIncarnation = 0;
	uint64_t leaseSequence = 0;
	uint64_t leaseDeadline = 0;
	uint64_t leaseCutoffReserve = 0;
	matocs::chunkserverSessionLease::deserialize(lease, leaseStableId, leaseIncarnation,
	                                             renewerMdsId, renewerMdsIncarnation, leaseSequence,
	                                             leaseDeadline, leaseCutoffReserve);
	EXPECT_EQ(leaseStableId, 7U);
	EXPECT_EQ(leaseIncarnation, 0x2122232425262728ULL);
	EXPECT_EQ(renewerMdsId, 3U);
	EXPECT_EQ(renewerMdsIncarnation, 0x1112131415161718ULL);
	EXPECT_EQ(leaseSequence, 6U);
	EXPECT_EQ(leaseDeadline, 10'020U);
	EXPECT_EQ(leaseCutoffReserve, 40U);
}

TEST(MatocsCommunicationTests, SetVersion) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 87, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 52, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, newVersion, 53, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocs::setVersion::serialize(buffer, chunkIdIn, chunkTypeIn, chunkVersionIn,
	                                              newVersionIn));

	verifyHeader(buffer, SAU_MATOCS_SET_VERSION);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, matocs::setVersion::kECChunks);
	ASSERT_NO_THROW(matocs::setVersion::deserialize(buffer, chunkIdOut, chunkTypeOut,
	                                                chunkVersionOut, newVersionOut));

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
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 87, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 52, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocs::deleteChunk::serialize(buffer, chunkIdIn, chunkTypeIn, chunkVersionIn));

	verifyHeader(buffer, SAU_MATOCS_DELETE_CHUNK);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(
	    matocs::deleteChunk::deserialize(buffer, chunkIdOut, chunkTypeOut, chunkVersionOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
}

TEST(MatocsCommunicationTests, Replicate) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 87, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 52, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_VECTOR_PAIR(ChunkTypeWithAddress, serverList) = {
	    ChunkTypeWithAddress(NetworkAddress(0xC0A80001, 8080), standard, SAUNAFS_VERSHEX),
	    ChunkTypeWithAddress(NetworkAddress(0xC0A80002, 8081), xor_p_of_6, SAUNAFS_VERSHEX),
	    ChunkTypeWithAddress(NetworkAddress(0xC0A80003, 8082), xor_1_of_6, SAUNAFS_VERSHEX),
	    ChunkTypeWithAddress(NetworkAddress(0xC0A80004, 8084), xor_5_of_7, SAUNAFS_VERSHEX),
	};

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(matocs::replicateChunk::serialize(buffer, chunkIdIn, chunkVersionIn,
	                                                  chunkTypeIn, serverListIn));

	verifyHeader(buffer, SAU_MATOCS_REPLICATE_CHUNK);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(matocs::replicateChunk::deserialize(buffer, chunkIdOut, chunkVersionOut,
	                                                    chunkTypeOut, serverListOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(serverList);
}
