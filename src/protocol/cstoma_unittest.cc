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
#include "protocol/cstoma.h"

#include <gtest/gtest.h>

#include "common/crc.h"
#include "errors/sfserr.h"
#include "unittests/chunk_type_constants.h"
#include "unittests/inout_pair.h"
#include "unittests/packet.h"

TEST(CstomaCommunicationTests, OverwriteStatusField) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 0xFFFFFFFFFFFFFFFF, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 0, 2);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::setVersion::serialize(buffer, chunkIdIn, chunkTypeIn, statusIn));
	statusIn = SAUNAFS_ERROR_WRONGOFFSET;
	cstoma::overwriteStatusField(buffer, statusIn);

	verifyHeader(buffer, SAU_CSTOMA_SET_VERSION);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstoma::setVersion::deserialize(buffer, chunkIdOut, chunkTypeOut, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
}

TEST(CstomaCommunicationTests, RegisterHost) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, ip, 127001, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint16_t, port, 8080, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, timeout, 100000, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, csVersion, SAUNAFS_VERSHEX, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::registerHost::serialize(buffer,
			ipIn, portIn, timeoutIn, csVersionIn));

	verifyHeader(buffer, SAU_CSTOMA_REGISTER_HOST);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstoma::registerHost::deserialize(buffer,
			ipOut, portOut, timeoutOut, csVersionOut));

	SAUNAFS_VERIFY_INOUT_PAIR(ip);
	SAUNAFS_VERIFY_INOUT_PAIR(port);
	SAUNAFS_VERIFY_INOUT_PAIR(timeout);
	SAUNAFS_VERIFY_INOUT_PAIR(csVersion);
}

TEST(CstomaCommunicationTests, RegisterPassive) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, ip, 127001, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint16_t, port, 8080, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, timeout, 100000, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, csVersion, SAUNAFS_VERSHEX, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(std::string, clusterId, "cluster-1", "");

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::registerPassive::serialize(buffer, ipIn, portIn, timeoutIn, csVersionIn,
	                                                   clusterIdIn));

	verifyHeader(buffer, SAU_CSTOMA_REGISTER_PASSIVE);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, 0);
	ASSERT_NO_THROW(cstoma::registerPassive::deserialize(buffer, ipOut, portOut, timeoutOut,
	                                                     csVersionOut, clusterIdOut));

	SAUNAFS_VERIFY_INOUT_PAIR(ip);
	SAUNAFS_VERIFY_INOUT_PAIR(port);
	SAUNAFS_VERIFY_INOUT_PAIR(timeout);
	SAUNAFS_VERIFY_INOUT_PAIR(csVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(clusterId);
}

TEST(CstomaCommunicationTests, RegisterChunks) {
	SAUNAFS_DEFINE_INOUT_VECTOR_PAIR(ChunkWithVersionAndType, chunks) = {
			ChunkWithVersionAndType(0, 1000, xor_1_of_3),
			ChunkWithVersionAndType(1, 1001, xor_7_of_7),
			ChunkWithVersionAndType(2, 1002, xor_p_of_4),
			ChunkWithVersionAndType(3, 1003, standard)
	};

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::registerChunks::serialize(buffer, chunksIn));

	verifyHeader(buffer, SAU_CSTOMA_REGISTER_CHUNKS);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstoma::registerChunks::deserialize(buffer, chunksOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunks);
}

TEST(CstomaCommunicationTests, RegisterSpace) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, usedSpace, 1, 2);
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, totalSpace, 3, 4);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunksNumber, 5, 6);
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, toDeleteUsedSpace, 7, 8);
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, toDeleteTotalSpace, 9, 10);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, toDeleteChunksNumber, 11, 12);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::registerSpace::serialize(buffer,
			usedSpaceIn, totalSpaceIn, chunksNumberIn,
			toDeleteUsedSpaceIn, toDeleteTotalSpaceIn, toDeleteChunksNumberIn));

	verifyHeader(buffer, SAU_CSTOMA_REGISTER_SPACE);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstoma::registerSpace::deserialize(buffer,
			usedSpaceOut, totalSpaceOut, chunksNumberOut,
			toDeleteUsedSpaceOut, toDeleteTotalSpaceOut, toDeleteChunksNumberOut));

	SAUNAFS_VERIFY_INOUT_PAIR(usedSpace);
	SAUNAFS_VERIFY_INOUT_PAIR(totalSpace);
	SAUNAFS_VERIFY_INOUT_PAIR(chunksNumber);
	SAUNAFS_VERIFY_INOUT_PAIR(toDeleteUsedSpace);
	SAUNAFS_VERIFY_INOUT_PAIR(toDeleteTotalSpace);
	SAUNAFS_VERIFY_INOUT_PAIR(toDeleteChunksNumber);
}

TEST(CstomaCommunicationTests, SetVersion) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 0xFFFFFFFFFFFFFFFF, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 2, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::setVersion::serialize(buffer, chunkIdIn, chunkTypeIn, statusIn));

	verifyHeader(buffer, SAU_CSTOMA_SET_VERSION);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstoma::setVersion::deserialize(buffer, chunkIdOut, chunkTypeOut, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
}

TEST(CstomaCommunicationTests, DeleteChunk) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 0xFFFFFFFFFFFFFFFF, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 2, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::deleteChunk::serialize(buffer, chunkIdIn, chunkTypeIn, statusIn));

	verifyHeader(buffer, SAU_CSTOMA_DELETE_CHUNK);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstoma::deleteChunk::deserialize(buffer, chunkIdOut, chunkTypeOut, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
}

TEST(CstomaCommunicationTests, ChunkLock) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 0xFFFFFFFFFFFFFFFF, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 2, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::chunkLock::serialize(buffer, chunkIdIn, chunkTypeIn, statusIn));

	verifyHeader(buffer, SAU_CSTOMA_LOCK_CHUNK);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstoma::chunkLock::deserialize(buffer, chunkIdOut, chunkTypeOut, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
}

TEST(CstomaCommunicationTests, WriteEndStatus) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 0xFFFFFFFFFFFFFFFF, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 2, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::writeEndStatus::serialize(buffer, chunkIdIn, chunkTypeIn, statusIn));

	verifyHeader(buffer, SAU_CSTOMA_WRITE_END_STATUS);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstoma::writeEndStatus::deserialize(buffer, chunkIdOut, chunkTypeOut, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
}

TEST(CstomaCommunicationTests, Replicate) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 0xFFFFFFFFFFFFFFFF, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 2, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 0x87654321, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::replicateChunk::serialize(buffer,
			chunkIdIn, chunkTypeIn, statusIn, chunkVersionIn));

	verifyHeader(buffer, SAU_CSTOMA_REPLICATE_CHUNK);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstoma::replicateChunk::deserialize(buffer,
			chunkIdOut, chunkTypeOut, statusOut, chunkVersionOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
}

TEST(CstomaCommunicationTests, Status) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, load, 77, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::status::serialize(buffer, loadIn));

	verifyHeader(buffer, SAU_CSTOMA_STATUS);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstoma::status::deserialize(buffer, loadOut));

	SAUNAFS_VERIFY_INOUT_PAIR(load);
}
