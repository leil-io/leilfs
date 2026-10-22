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
#include "protocol/cstoma.h"

#include <array>

#include <gtest/gtest.h>

#include "common/crc.h"
#include "common/datapack.h"
#include "errors/sfserr.h"
#include "unittests/chunk_type_constants.h"
#include "unittests/inout_pair.h"
#include "unittests/packet.h"

TEST(CstomaCommunicationTests, ChunkserverId) {
	const std::array<uint8_t, 16> idIn = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	                                      0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe};
	std::array<uint8_t, 16> idOut{};

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::chunkserverId::serialize(buffer, idIn));

	verifyHeader(buffer, SAU_CSTOMA_CHUNKSERVER_ID);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, cstoma::chunkserverId::kDefault);
	ASSERT_NO_THROW(cstoma::chunkserverId::deserialize(buffer, idOut));
	EXPECT_EQ(idOut, idIn);
}

TEST(CstomaCommunicationTests, ChunkserverIdRejectsWrongLength) {
	const std::array<uint8_t, 16> id{};
	std::array<uint8_t, 16> parsed{};
	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::chunkserverId::serialize(buffer, id));
	removeHeaderInPlace(buffer);

	buffer.pop_back();
	EXPECT_ANY_THROW(cstoma::chunkserverId::deserialize(buffer, parsed));
	buffer.push_back(0);
	buffer.push_back(0);
	EXPECT_ANY_THROW(cstoma::chunkserverId::deserialize(buffer, parsed));
}

TEST(CstomaCommunicationTests, ChunkserverIdRejectsWrongVersion) {
	const std::array<uint8_t, 16> id{};
	std::array<uint8_t, 16> parsed{};
	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::chunkserverId::serialize(buffer, id));
	removeHeaderInPlace(buffer);

	uint8_t *version = buffer.data();
	put32bit(&version, cstoma::chunkserverId::kDefault + 1);
	EXPECT_ANY_THROW(cstoma::chunkserverId::deserialize(buffer, parsed));
}

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

TEST(CstomaCommunicationTests, ProbeChunk) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 0xFFFFFFFFFFFFFFFF, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 52, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 2, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(
	    cstoma::probeChunk::serialize(buffer, chunkIdIn, chunkTypeIn, chunkVersionIn, statusIn));

	verifyHeader(buffer, SAU_CSTOMA_PROBE_CHUNK);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstoma::probeChunk::deserialize(buffer, chunkIdOut, chunkTypeOut,
	                                                chunkVersionOut, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
}

TEST(CstomaCommunicationTests, MaintenanceFence) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, epoch, 0x1122334455667788, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 2, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::maintenanceFence::serialize(buffer, epochIn, statusIn));

	verifyHeader(buffer, SAU_CSTOMA_MAINTENANCE_FENCE);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstoma::maintenanceFence::deserialize(buffer, epochOut, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(epoch);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
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

// The id-free reply families must keep their exact bytes, so Master and Shadow are unchanged.
TEST(CstomaCommunicationTests, OldFamiliesAreByteIdentical) {
	std::vector<uint8_t> deletePacket;
	cstoma::deleteChunk::serialize(deletePacket, uint64_t(87), xor_p_of_3, uint8_t(2));
	EXPECT_EQ(deletePacket, (std::vector<uint8_t>{0x00, 0x00, 0x04, 0x61, 0x00, 0x00, 0x00, 0x0f,
	                                              0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	                                              0x00, 0x00, 0x00, 0x57, 0x00, 0xc0, 0x02}));

	std::vector<uint8_t> probePacket;
	cstoma::probeChunk::serialize(probePacket, uint64_t(87), xor_p_of_3, uint32_t(52), uint8_t(2));
	EXPECT_EQ(probePacket,
	          (std::vector<uint8_t>{0x00, 0x00, 0x04, 0x9b, 0x00, 0x00, 0x00, 0x13, 0x00,
	                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	                                0x00, 0x57, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x34, 0x02}));

	std::vector<uint8_t> replicatePacket;
	cstoma::replicateChunk::serialize(replicatePacket, uint64_t(87), xor_p_of_3, uint8_t(2),
	                                  uint32_t(52));
	EXPECT_EQ(replicatePacket,
	          (std::vector<uint8_t>{0x00, 0x00, 0x04, 0x7f, 0x00, 0x00, 0x00, 0x13, 0x00,
	                                0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	                                0x00, 0x57, 0x00, 0xc0, 0x02, 0x00, 0x00, 0x00, 0x34}));
}

TEST(CstomaCommunicationTests, DeleteChunkWithRequestId) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 87, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 2, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, requestId, 0x1122334455667788, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(
	    cstoma::deleteChunk::serialize(buffer, chunkIdIn, chunkTypeIn, statusIn, requestIdIn));
	verifyHeader(buffer, SAU_CSTOMA_DELETE_CHUNK);
	removeHeaderInPlace(buffer);
	PacketVersion version = 0;
	deserializePacketVersionNoHeader(buffer, version);
	ASSERT_EQ(version, cstoma::deleteChunk::kWithRequestId);
	ASSERT_NO_THROW(cstoma::deleteChunk::deserialize(buffer, chunkIdOut, chunkTypeOut, statusOut,
	                                                 requestIdOut));
	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
	SAUNAFS_VERIFY_INOUT_PAIR(requestId);
}

TEST(CstomaCommunicationTests, ProbeChunkWithRequestId) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 87, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 52, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 2, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, requestId, 0x1122334455667788, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::probeChunk::serialize(buffer, chunkIdIn, chunkTypeIn, chunkVersionIn,
	                                              statusIn, requestIdIn));
	verifyHeader(buffer, SAU_CSTOMA_PROBE_CHUNK);
	removeHeaderInPlace(buffer);
	PacketVersion version = 0;
	deserializePacketVersionNoHeader(buffer, version);
	ASSERT_EQ(version, cstoma::probeChunk::kWithRequestId);
	ASSERT_NO_THROW(cstoma::probeChunk::deserialize(buffer, chunkIdOut, chunkTypeOut,
	                                                chunkVersionOut, statusOut, requestIdOut));
	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
	SAUNAFS_VERIFY_INOUT_PAIR(requestId);
}

TEST(CstomaCommunicationTests, TestChunk) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 87, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, testedVersion, 52, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 2, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, requestId, 0x1122334455667788, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::testChunk::serialize(buffer, chunkIdIn, chunkTypeIn, testedVersionIn,
	                                             statusIn, requestIdIn));
	verifyHeader(buffer, SAU_CSTOMA_TEST_CHUNK);
	removeHeaderInPlace(buffer);
	verifyVersion(buffer, cstoma::testChunk::kDefault);
	ASSERT_NO_THROW(cstoma::testChunk::deserialize(buffer, chunkIdOut, chunkTypeOut,
	                                               testedVersionOut, statusOut, requestIdOut));
	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(testedVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
	SAUNAFS_VERIFY_INOUT_PAIR(requestId);
}

TEST(CstomaCommunicationTests, ReplicateChunkWithRequestId) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 87, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_3, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 2, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 52, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, requestId, 0x1122334455667788, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstoma::replicateChunk::serialize(buffer, chunkIdIn, chunkTypeIn, statusIn,
	                                                  chunkVersionIn, requestIdIn));
	verifyHeader(buffer, SAU_CSTOMA_REPLICATE_CHUNK);

	// Status stays third so overwriteStatusField still reaches it in the new reply.
	cstoma::overwriteStatusField(buffer, 5);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstoma::replicateChunk::deserialize(buffer, chunkIdOut, chunkTypeOut, statusOut,
	                                                    chunkVersionOut, requestIdOut));
	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	EXPECT_EQ(statusOut, 5);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(requestId);
}
