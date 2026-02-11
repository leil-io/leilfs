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
