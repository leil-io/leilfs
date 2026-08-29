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

#pragma once

#include "common/platform.h"

#include "common/serialization_macros.h"
#include "common/chunk_part_type.h"
#include "common/chunk_type_with_address.h"
#include "common/network_address.h"
#include "protocol/packet.h"

SAUNAFS_DEFINE_PACKET_VERSION(cltocs, prefetch, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cltocs, prefetch, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltocs, prefetch, SAU_CLTOCS_PREFETCH, kStandardAndXorChunks,
		uint64_t, chunkId, uint32_t, chunkVersion, legacy::ChunkPartType, chunkType,
		uint32_t, readOffset, uint32_t, readSize)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltocs, prefetch, SAU_CLTOCS_PREFETCH, kECChunks,
		uint64_t, chunkId, uint32_t, chunkVersion, ChunkPartType, chunkType,
		uint32_t, readOffset, uint32_t, readSize)

namespace cltocs {

namespace read {

const PacketVersion kStandardAndXorChunks = 0;
const PacketVersion kECChunks = 1;

inline void serialize(std::vector<uint8_t>& destination,
		uint64_t chunkId, uint32_t chunkVersion, legacy::ChunkPartType chunkType,
		uint32_t readOffset, uint32_t readSize) {
	serializePacket(destination, SAU_CLTOCS_READ, kStandardAndXorChunks,
			chunkId, chunkVersion, chunkType, readOffset, readSize);
}

inline void deserialize(const uint8_t* source, uint32_t sourceSize,
		uint64_t& chunkId, uint32_t& chunkVersion, legacy::ChunkPartType& chunkType,
		uint32_t& readOffset, uint32_t& readSize) {
	verifyPacketVersionNoHeader(source, sourceSize, kStandardAndXorChunks);
	deserializeAllPacketDataNoHeader(source, sourceSize,
			chunkId, chunkVersion, chunkType, readOffset, readSize);
}

inline void serialize(std::vector<uint8_t>& destination,
		uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
		uint32_t readOffset, uint32_t readSize) {
	serializePacket(destination, SAU_CLTOCS_READ, kECChunks,
			chunkId, chunkVersion, chunkType, readOffset, readSize);
}

inline void deserialize(const uint8_t* source, uint32_t sourceSize,
		uint64_t& chunkId, uint32_t& chunkVersion, ChunkPartType& chunkType,
		uint32_t& readOffset, uint32_t& readSize) {
	verifyPacketVersionNoHeader(source, sourceSize, kECChunks);
	deserializeAllPacketDataNoHeader(source, sourceSize,
			chunkId, chunkVersion, chunkType, readOffset, readSize);
}

} // namespace read

namespace writeInit {

const PacketVersion kECChunks = 1;
const PacketVersion kECChunksWithWriteFlush = 2;
const PacketVersion kECChunksWithGrant = 3;

inline void serialize(std::vector<uint8_t>& destination,
		uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
		const std::vector<ChunkTypeWithAddress>& chain) {
	serializePacket(destination, SAU_CLTOCS_WRITE_INIT, kECChunks,
			chunkId, chunkVersion, chunkType, chain);
}

inline void deserialize(const uint8_t* source, uint32_t sourceSize,
		uint64_t& chunkId, uint32_t& chunkVersion, ChunkPartType& chunkType,
		std::vector<ChunkTypeWithAddress>& chain) {
	verifyPacketVersionNoHeader(source, sourceSize, kECChunks);
	deserializeAllPacketDataNoHeader(source, sourceSize,
			chunkId, chunkVersion, chunkType, chain);
}

inline void serialize(std::vector<uint8_t> &destination, uint64_t chunkId, uint32_t chunkVersion,
                      ChunkPartType chunkType, bool expectWriteFlush,
                      const std::vector<ChunkTypeWithAddress> &chain) {
	serializePacket(destination, SAU_CLTOCS_WRITE_INIT, kECChunksWithWriteFlush,
			chunkId, chunkVersion, chunkType, expectWriteFlush, chain);
}

inline void deserialize(const uint8_t *source, uint32_t sourceSize, uint64_t &chunkId,
                        uint32_t &chunkVersion, ChunkPartType &chunkType, bool &expectWriteFlush,
                        std::vector<ChunkTypeWithAddress> &chain) {
	verifyPacketVersionNoHeader(source, sourceSize, kECChunksWithWriteFlush);
	deserializeAllPacketDataNoHeader(source, sourceSize,
			chunkId, chunkVersion, chunkType, expectWriteFlush, chain);
}

inline void serialize(std::vector<uint8_t> &destination, uint64_t chunkId, uint32_t chunkVersion,
                      ChunkPartType chunkType, bool expectWriteFlush, uint64_t grantGeneration,
                      uint64_t grantRandom, const std::vector<ChunkTypeWithAddress> &chain) {
	serializePacket(destination, SAU_CLTOCS_WRITE_INIT, kECChunksWithGrant, chunkId, chunkVersion,
	                chunkType, expectWriteFlush, grantGeneration, grantRandom, chain);
}

inline void deserialize(const uint8_t *source, uint32_t sourceSize, uint64_t &chunkId,
                        uint32_t &chunkVersion, ChunkPartType &chunkType, bool &expectWriteFlush,
                        uint64_t &grantGeneration, uint64_t &grantRandom,
                        std::vector<ChunkTypeWithAddress> &chain) {
	verifyPacketVersionNoHeader(source, sourceSize, kECChunksWithGrant);
	deserializeAllPacketDataNoHeader(source, sourceSize, chunkId, chunkVersion, chunkType,
	                                 expectWriteFlush, grantGeneration, grantRandom, chain);
}

} // namespace writeInit

namespace writeData {

inline void serializePrefix(std::vector<uint8_t>& destination,
		uint64_t chunkId, uint32_t writeId,
		uint16_t blockNumber, uint32_t offset, uint32_t size, uint32_t crc) {
	serializePacketPrefix(destination, size, SAU_CLTOCS_WRITE_DATA, 0,
			chunkId, writeId, blockNumber, offset, size, crc);
}

inline void deserializePrefix(const uint8_t* source, uint32_t sourceSize,
		uint64_t& chunkId, uint32_t& writeId,
		uint16_t& blockNumber, uint32_t& offset, uint32_t& size, uint32_t& crc) {
	verifyPacketVersionNoHeader(source, sourceSize, 0);
	deserializePacketDataNoHeader(source, sourceSize,
			chunkId, writeId, blockNumber, offset, size, crc);
}

// kPrefixSize is equal to:
// version:u32 chunkId:u64 writeId:32 block:16 offset:32 size:32 crc:32
static const uint32_t kPrefixSize = 4 + 8 + 4 + 2 + 4 + 4 + 4;

} // namespace writeData

namespace writeFlush {

const PacketVersion kInitial = 0;

inline void serialize(std::vector<uint8_t> &destination, uint64_t chunkId) {
	serializePacket(destination, SAU_CLTOCS_WRITE_FLUSH, kInitial, chunkId);
}

inline void deserialize(const uint8_t *source, uint32_t sourceSize, uint64_t &chunkId) {
	verifyPacketVersionNoHeader(source, sourceSize, kInitial);
	deserializePacketDataNoHeader(source, sourceSize, chunkId);
}

// kPrefixSize is equal to:
// version:u32 chunkId:u64
static const uint32_t kPrefixSize = 4 + 8;

} // namespace writeFlush

namespace writeEnd {

inline void serialize(std::vector<uint8_t>& destination, uint64_t chunkId) {
	serializePacket(destination, SAU_CLTOCS_WRITE_END, 0, chunkId);
}

inline void deserialize(const uint8_t* source, uint32_t sourceSize, uint64_t& chunkId) {
	verifyPacketVersionNoHeader(source, sourceSize, 0);
	deserializeAllPacketDataNoHeader(source, sourceSize, chunkId);
}

} // namespace writeEnd

namespace testChunk {

const PacketVersion kStandardAndXorChunks = 0;
const PacketVersion kECChunks = 1;

inline void serialize(std::vector<uint8_t>& destination,
		uint64_t chunkId, uint32_t chunkVersion, legacy::ChunkPartType chunkType) {
	serializePacket(destination, SAU_CLTOCS_TEST_CHUNK, kStandardAndXorChunks, chunkId, chunkVersion, chunkType);
}

inline void deserialize(const uint8_t* source, uint32_t sourceSize,
		uint64_t& chunkId, uint32_t& chunkVersion, legacy::ChunkPartType& chunkType) {
	verifyPacketVersionNoHeader(source, sourceSize, kStandardAndXorChunks);
	deserializeAllPacketDataNoHeader(source, sourceSize, chunkId, chunkVersion, chunkType);
}

inline void serialize(std::vector<uint8_t>& destination,
		uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType) {
	serializePacket(destination, SAU_CLTOCS_TEST_CHUNK, kECChunks, chunkId, chunkVersion, chunkType);
}

inline void deserialize(const uint8_t* source, uint32_t sourceSize,
		uint64_t& chunkId, uint32_t& chunkVersion, ChunkPartType& chunkType) {
	verifyPacketVersionNoHeader(source, sourceSize, kECChunks);
	deserializeAllPacketDataNoHeader(source, sourceSize, chunkId, chunkVersion, chunkType);
}

} // namespace testChunk

} // namespace cltocs
