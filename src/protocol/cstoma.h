/*
   Copyright 2013-2014 EditShare
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

#include <iostream>

#include "common/chunk_command_identity.h"
#include "common/chunk_part_type.h"
#include "common/chunk_with_version.h"
#include "common/chunk_with_version_and_type.h"
#include "common/serialization_macros.h"
#include "protocol/chunks_with_type.h"
#include "protocol/packet.h"

namespace cstoma {
inline void overwriteStatusField(std::vector<uint8_t> &destination, uint8_t status) {
	// 9 - sizeof chunkId + chunkType, 1 - sizeof status
	uint32_t statusOffset = PacketHeader::kSize + serializedSize(PacketVersion()) +
	                        sizeof(uint64_t) + serializedSize(ChunkPartType());
	sassert(destination.size() >= statusOffset + 1);
	destination[statusOffset] = status;
}
}  // namespace cstoma

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, chunkNew, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cstoma, chunkNew, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, chunkNew, SAU_CSTOMA_CHUNK_NEW, kStandardAndXorChunks,
                                    std::vector<legacy::ChunkWithVersionAndType>, chunks)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, chunkNew, SAU_CSTOMA_CHUNK_NEW, kECChunks,
                                    std::vector<ChunkWithVersionAndType>, chunks)

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, registerHost, kWithoutClusterId, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cstoma, registerHost, kWithClusterId, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, registerHost, SAU_CSTOMA_REGISTER_HOST,
                                    kWithoutClusterId, uint32_t, ip, uint16_t, port, uint32_t,
                                    timeout, uint32_t, csVersion)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, registerHost, SAU_CSTOMA_REGISTER_HOST, kWithClusterId,
                                    uint32_t, ip, uint16_t, port, uint32_t, timeout, uint32_t,
                                    csVersion, std::string, clusterId)

// The distributed handshake is a distinct packet, never a version of legacy registerHost.
// role is DistributedRegistrationRole encoded as u8. Stable id zero is valid only with
// MINT_ONLY; every request still carries the process incarnation so a delayed reply is tied
// to one process start. readiness/scanEpoch are connection facts, not claim authority.
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, registerDistributed, SAU_CSTOMA_REGISTER_DISTRIBUTED, 0,
                                    uint32_t, ip, uint16_t, port, uint32_t, timeout, uint32_t,
                                    csVersion, std::string, clusterId, uint32_t, stableId, uint64_t,
                                    chunkserverIncarnation, uint8_t, readiness, uint64_t, scanEpoch,
                                    uint8_t, role)

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, registerChunks, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cstoma, registerChunks, kStandardChunksOnly, 1)
SAUNAFS_DEFINE_PACKET_VERSION(cstoma, registerChunks, kECChunks, 2)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, registerChunks, SAU_CSTOMA_REGISTER_CHUNKS,
                                    kStandardAndXorChunks,
                                    std::vector<legacy::ChunkWithVersionAndType>, chunks)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, registerChunks, SAU_CSTOMA_REGISTER_CHUNKS,
                                    kStandardChunksOnly, std::vector<ChunkWithVersion>, chunks)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, registerChunks, SAU_CSTOMA_REGISTER_CHUNKS, kECChunks,
                                    std::vector<ChunkWithVersionAndType>, chunks)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, registerSpace, SAU_CSTOMA_REGISTER_SPACE, 0, uint64_t,
                                    usedSpace, uint64_t, totalSpace, uint32_t, chunkCount, uint64_t,
                                    tdUsedSpace, uint64_t, toDeleteTotalSpace, uint32_t,
                                    toDeleteChunksNumber)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, registerLabel, SAU_CSTOMA_REGISTER_LABEL, 0,
                                    std::string, label)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, registerConfig, SAU_CSTOMA_REGISTER_CONFIG, 0,
                                    std::string, config)

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, setVersion, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cstoma, setVersion, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, setVersion, SAU_CSTOMA_SET_VERSION,
                                    kStandardAndXorChunks, uint64_t, chunkId, legacy::ChunkPartType,
                                    chunkType, uint8_t, status)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, setVersion, SAU_CSTOMA_SET_VERSION, kECChunks, uint64_t,
                                    chunkId, ChunkPartType, chunkType, uint8_t, status)

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, chunkLock, kECChunks, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, chunkLock, SAU_CSTOMA_LOCK_CHUNK, kECChunks, uint64_t,
                                    chunkId, ChunkPartType, chunkType, uint8_t, status)

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, writeEndStatus, kECChunks, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, writeEndStatus, SAU_CSTOMA_WRITE_END_STATUS, kECChunks,
                                    uint64_t, chunkId, ChunkPartType, chunkType, uint8_t, status)

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, deleteChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cstoma, deleteChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, deleteChunk, SAU_CSTOMA_DELETE_CHUNK,
                                    kStandardAndXorChunks, uint64_t, chunkId, legacy::ChunkPartType,
                                    chunkType, uint8_t, status)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, deleteChunk, SAU_CSTOMA_DELETE_CHUNK, kECChunks,
                                    uint64_t, chunkId, ChunkPartType, chunkType, uint8_t, status)

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, createChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cstoma, createChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, createChunk, SAU_CSTOMA_CREATE_CHUNK,
                                    kStandardAndXorChunks, uint64_t, chunkId, legacy::ChunkPartType,
                                    chunkType, uint8_t, status)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, createChunk, SAU_CSTOMA_CREATE_CHUNK, kECChunks,
                                    uint64_t, chunkId, ChunkPartType, chunkType, uint8_t, status)

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, truncate, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cstoma, truncate, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, truncate, SAU_CSTOMA_TRUNCATE, kStandardAndXorChunks,
                                    uint64_t, chunkId, legacy::ChunkPartType, chunkType, uint8_t,
                                    status)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, truncate, SAU_CSTOMA_TRUNCATE, kECChunks, uint64_t,
                                    chunkId, ChunkPartType, chunkType, uint8_t, status)

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, duplicateChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cstoma, duplicateChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, duplicateChunk, SAU_CSTOMA_DUPLICATE_CHUNK,
                                    kStandardAndXorChunks, uint64_t, chunkId, legacy::ChunkPartType,
                                    chunkType, uint8_t, status)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, duplicateChunk, SAU_CSTOMA_DUPLICATE_CHUNK, kECChunks,
                                    uint64_t, chunkId, ChunkPartType, chunkType, uint8_t, status)

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, duptruncChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cstoma, duptruncChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, duptruncChunk, SAU_CSTOMA_DUPTRUNC_CHUNK,
                                    kStandardAndXorChunks, uint64_t, chunkId, legacy::ChunkPartType,
                                    chunkType, uint8_t, status)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, duptruncChunk, SAU_CSTOMA_DUPTRUNC_CHUNK, kECChunks,
                                    uint64_t, chunkId, ChunkPartType, chunkType, uint8_t, status)

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, replicateChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cstoma, replicateChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
    cstoma, replicateChunk, SAU_CSTOMA_REPLICATE_CHUNK, kStandardAndXorChunks, uint64_t, chunkId,
    legacy::ChunkPartType, chunkType, uint8_t,
    status,  // status has to be third field to make overwriteStatusField work!!!
    uint32_t, chunkVersion)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
    cstoma, replicateChunk, SAU_CSTOMA_REPLICATE_CHUNK, kECChunks, uint64_t, chunkId, ChunkPartType,
    chunkType, uint8_t,
    status,  // status has to be third field to make overwriteStatusField work!!!
    uint32_t, chunkVersion)

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, chunkDamaged, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cstoma, chunkDamaged, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, chunkDamaged, SAU_CSTOMA_CHUNK_DAMAGED,
                                    kStandardAndXorChunks, std::vector<legacy::ChunkWithType>,
                                    chunks)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, chunkDamaged, SAU_CSTOMA_CHUNK_DAMAGED, kECChunks,
                                    std::vector<ChunkWithType>, chunks)

SAUNAFS_DEFINE_PACKET_VERSION(cstoma, chunkLost, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cstoma, chunkLost, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, chunkLost, SAU_CSTOMA_CHUNK_LOST, kStandardAndXorChunks,
                                    std::vector<legacy::ChunkWithType>, chunks)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, chunkLost, SAU_CSTOMA_CHUNK_LOST, kECChunks,
                                    std::vector<ChunkWithType>, chunks)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, status, SAU_CSTOMA_STATUS, 0, uint8_t, load)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, startTls, SAU_CSTOMA_STARTTLS, 0)

// The one reply of the fenced command plane. Every fenced command answers with this, so a
// participant that cannot execute a command still has a shape to say so in, and the metadata
// server has one place where a result is matched rather than nine.
//
// chunkId, chunkType and status stay in that order and in those positions so that
// cstoma::overwriteStatusField keeps working: the reply is built when the command is accepted
// and its status is stamped in when the work finishes, which is what lets the identity be
// captured at accept time rather than carried through the job pool.
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cstoma, fencedStatus, SAU_CSTOMA_FENCED_STATUS, 0, uint64_t,
                                    chunkId, ChunkPartType, chunkType, uint8_t, status,
                                    ChunkCommandIdentity, identity, uint8_t, family, uint32_t,
                                    resultVersion)
