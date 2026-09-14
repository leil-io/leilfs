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

#pragma once

#include "common/platform.h"

#include "common/chunk_type_with_address.h"
#include "common/metadata_cluster_member.h"
#include "common/serialization_macros.h"
#include "protocol/SFSCommunication.h"
#include "protocol/packet.h"

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocs, registerHost, SAU_MATOCS_REGISTER_HOST, 0,
		uint8_t, status,
		uint32_t, version,
		std::string, clusterId)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, requestChunkserverId, kDefault, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, requestChunkserverId, SAU_MATOCS_REQUEST_CHUNKSERVER_ID,
                                    kDefault)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, clusterMembers, kDefault, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, clusterMembers, SAU_MATOCS_CLUSTER_MEMBERS, kDefault,
                                    uint32_t, seedId, std::vector<MetadataClusterMember>, members)

namespace matocs::clusterMembers {

/// Deserializes with the size checked first, so a hostile or corrupt length cannot drive the
/// vector deserializer into a huge allocation.
/// @throws IncorrectDeserializationException on a truncated, oversized or invalid snapshot.
inline MetadataClusterSnapshot readSnapshot(const std::vector<uint8_t> &data) {
	// Wire layout: version (4) + seedId (4) + count (4); then per member serverId (4) + ip (4) +
	// port (2) + version (4).
	constexpr size_t kHeaderSize = 12;
	constexpr size_t kMemberSize = 14;
	if (data.size() < kHeaderSize) {
		throw IncorrectDeserializationException("truncated cluster discovery packet");
	}
	const uint8_t *countField = data.data() + 8;
	uint32_t count = 0;
	get32bit(&countField, count);
	if (count >= kMaxMetadataConnections || data.size() != kHeaderSize + count * kMemberSize) {
		throw IncorrectDeserializationException("invalid cluster discovery size");
	}

	MetadataClusterSnapshot snapshot;
	deserialize(data, snapshot.seedId, snapshot.members);
	if (!isValidMetadataClusterSnapshot(snapshot.seedId, snapshot.members)) {
		throw IncorrectDeserializationException("invalid cluster discovery members");
	}
	return snapshot;
}

}  // namespace matocs::clusterMembers

SAUNAFS_DEFINE_PACKET_VERSION(matocs, probeChunk, kDefault, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, probeChunk, SAU_MATOCS_PROBE_CHUNK, kDefault, uint64_t,
                                    chunkId, ChunkPartType, chunkType)

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
