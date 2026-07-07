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

#include "common/chunk_type_with_address.h"
#include "protocol/SFSCommunication.h"
#include "protocol/packet.h"
#include "common/serialization_macros.h"

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, registerHost, SAU_MATOCS_REGISTER_HOST, 0,
		uint8_t, status,
		uint32_t, version,
		std::string, clusterId)

// On-demand chunk-location query: while a chunkserver's registration is in
// progress, the master asks which of these chunks the chunkserver hosts so
// that waiting client operations can be answered without waiting for the
// full registration. Answered with SAU_CSTOMA_QUERY_CHUNKS_RESPONSE.
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, queryChunks, SAU_MATOCS_QUERY_CHUNKS, 0,
                                    std::vector<uint64_t>, chunkIds)

// Master-driven (pull) chunk registration: tells a freshly host-registered
// chunkserver to start sending its chunk list in bulks of at most bulkSize
// chunks, with at most initialCredits bulks in flight. Further bulks are
// released by SAU_MATOCS_REGISTER_CHUNKS_CREDIT grants; the chunkserver ends
// the stream with SAU_CSTOMA_REGISTER_CHUNKS_END. This lets the master pace
// registration during a mass reconnect (e.g. after a failover).
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, registerChunksStart, SAU_MATOCS_REGISTER_CHUNKS_START,
                                    0, uint32_t, bulkSize, uint32_t, initialCredits)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, registerChunksCredit, SAU_MATOCS_REGISTER_CHUNKS_CREDIT,
                                    0, uint32_t, credits)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, setVersion, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, setVersion, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, setVersion, SAU_MATOCS_SET_VERSION, kStandardAndXorChunks,
		uint64_t,  chunkId,
		legacy::ChunkPartType, chunkType,
		uint32_t,  chunkVersion,
		uint32_t,  newVersion)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, setVersion, SAU_MATOCS_SET_VERSION, kECChunks,
		uint64_t,  chunkId,
		ChunkPartType, chunkType,
		uint32_t,  chunkVersion,
		uint32_t,  newVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, setVersionAndLock, kECChunks, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, setVersionAndLock, SAU_MATOCS_SET_VERSION_AND_LOCK, kECChunks,
		uint64_t,  chunkId,
		ChunkPartType, chunkType,
		uint32_t,  chunkVersion,
		uint32_t,  newVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, chunkLock, kECChunks, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, chunkLock, SAU_MATOCS_LOCK_CHUNK, kECChunks,
		uint64_t,  chunkId,
		ChunkPartType, chunkType)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, chunkUnlock, kECChunks, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
	matocs, chunkUnlock, SAU_MATOCS_UNLOCK_CHUNK, kECChunks,
	uint64_t,  chunkId,
	ChunkPartType, chunkType)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, deleteChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, deleteChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, deleteChunk, SAU_MATOCS_DELETE_CHUNK, kStandardAndXorChunks,
		uint64_t,  chunkId,
		legacy::ChunkPartType, chunkType,
		uint32_t,  chunkVersion)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, deleteChunk, SAU_MATOCS_DELETE_CHUNK, kECChunks,
		uint64_t,  chunkId,
		ChunkPartType, chunkType,
		uint32_t,  chunkVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, createChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, createChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, createChunk, SAU_MATOCS_CREATE_CHUNK, kStandardAndXorChunks,
		uint64_t,  chunkId,
		legacy::ChunkPartType, chunkType,
		uint32_t,  chunkVersion)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, createChunk, SAU_MATOCS_CREATE_CHUNK, kECChunks,
		uint64_t,  chunkId,
		ChunkPartType, chunkType,
		uint32_t,  chunkVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, createAndLockChunk, kECChunks, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, createAndLockChunk, SAU_MATOCS_CREATE_AND_LOCK_CHUNK, kECChunks,
		uint64_t,  chunkId,
		ChunkPartType, chunkType,
		uint32_t,  chunkVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, truncateChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, truncateChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, truncateChunk, SAU_MATOCS_TRUNCATE, kStandardAndXorChunks,
		uint64_t,  chunkId,
		legacy::ChunkPartType, chunkType,
		uint32_t,  length, // if xor chunk - length of chunk part
		uint32_t,  newVersion,
		uint32_t,  oldVersion)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, truncateChunk, SAU_MATOCS_TRUNCATE, kECChunks,
		uint64_t,  chunkId,
		ChunkPartType, chunkType,
		uint32_t,  length, // if xor chunk - length of chunk part
		uint32_t,  newVersion,
		uint32_t,  oldVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, duplicateChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, duplicateChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, duplicateChunk, SAU_MATOCS_DUPLICATE_CHUNK, kStandardAndXorChunks,
		uint64_t, newChunkId,
		uint32_t, newchunkVersion,
		legacy::ChunkPartType, chunkType,
		uint64_t, oldChunkId,
		uint32_t, oldChunkVersion)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, duplicateChunk, SAU_MATOCS_DUPLICATE_CHUNK, kECChunks,
		uint64_t, newChunkId,
		uint32_t, newchunkVersion,
		ChunkPartType, chunkType,
		uint64_t, oldChunkId,
		uint32_t, oldChunkVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, duplicateAndLockChunk, kECChunks, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, duplicateAndLockChunk, SAU_MATOCS_DUPLICATE_AND_LOCK_CHUNK, kECChunks,
		uint64_t, newChunkId,
		uint32_t, newChunkVersion,
		ChunkPartType, chunkType,
		uint64_t, oldChunkId,
		uint32_t, oldChunkVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, duptruncChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, duptruncChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, duptruncChunk, SAU_MATOCS_DUPTRUNC_CHUNK, kStandardAndXorChunks,
		uint64_t, newChunkId,
		uint32_t, newchunkVersion,
		legacy::ChunkPartType, chunkType,
		uint64_t, oldChunkId,
		uint32_t, oldChunkVersion,
		uint32_t, length) // if xor chunk - length of chunk part
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, duptruncChunk, SAU_MATOCS_DUPTRUNC_CHUNK, kECChunks,
		uint64_t, newChunkId,
		uint32_t, newchunkVersion,
		ChunkPartType, chunkType,
		uint64_t, oldChunkId,
		uint32_t, oldChunkVersion,
		uint32_t, length) // if xor chunk - length of chunk part

SAUNAFS_DEFINE_PACKET_VERSION(matocs, replicateChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, replicateChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, replicateChunk, SAU_MATOCS_REPLICATE_CHUNK, kStandardAndXorChunks,
		uint64_t,  chunkId,
		uint32_t,  chunkVersion,
		legacy::ChunkPartType, chunkType,
		std::vector<legacy::ChunkTypeWithAddress>, sources)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, replicateChunk, SAU_MATOCS_REPLICATE_CHUNK, kECChunks,
		uint64_t,  chunkId,
		uint32_t,  chunkVersion,
		ChunkPartType, chunkType,
		std::vector<ChunkTypeWithAddress>, sources)

namespace matocs {
namespace replicateChunk {

inline void deserializePartial(const std::vector<uint8_t>& source,
		uint64_t& chunkId, uint32_t& chunkVersion, legacy::ChunkPartType& chunkType, const uint8_t*& sources) {
	verifyPacketVersionNoHeader(source, kStandardAndXorChunks);
	deserializeAllPacketDataNoHeader(source, chunkId, chunkVersion, chunkType, sources);
}

inline void deserializePartial(const std::vector<uint8_t>& source,
		uint64_t& chunkId, uint32_t& chunkVersion, ChunkPartType& chunkType, const uint8_t*& sources) {
	verifyPacketVersionNoHeader(source, kECChunks);
	deserializeAllPacketDataNoHeader(source, chunkId, chunkVersion, chunkType, sources);
}

} // namespace replicate
} // namespace matocs
