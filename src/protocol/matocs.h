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
#include "common/metadataserver_cluster_entry.h"
#include "common/serialization_macros.h"
#include "protocol/SFSCommunication.h"
#include "protocol/packet.h"

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, registerHost, SAU_MATOCS_REGISTER_HOST, 0,
		uint8_t, status,
		uint32_t, version,
		std::string, clusterId)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, clusterMembers, SAU_MATOCS_CLUSTER_MEMBERS, 0,
                                    std::vector<MetadataserverClusterEntry>, members)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, registerPassive, SAU_MATOCS_REGISTER_PASSIVE, 0,
                                    uint8_t, status, uint32_t, version, std::string, clusterId)

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
