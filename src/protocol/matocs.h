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
#include "common/chunk_command_identity.h"
#include "common/metadataserver_cluster_entry.h"
#include "common/serialization_macros.h"
#include "protocol/SFSCommunication.h"
#include "protocol/packet.h"

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, registerHost, SAU_MATOCS_REGISTER_HOST, 0, uint8_t,
                                    status, uint32_t, version, std::string, clusterId)

// Reply to the distinct distributed registration. A successful mint-only reply has a
// nonzero stableId and zero claimSequence/deadline; the chunkserver must persist the id and
// reconnect with CLAIM_RENEWER. An admitted reply has all identity and claim fields nonzero.
// cutoffReserveSeconds is the MDS-configured clock tolerance plus drain bound, so both
// sides derive the same conservative cutoff from one durable deadline.
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, registerDistributed, SAU_MATOCS_REGISTER_DISTRIBUTED, 0,
                                    uint8_t, status, uint32_t, stableId, uint32_t, mdsId, uint64_t,
                                    mdsIncarnation, uint32_t, version, std::string, clusterId,
                                    uint64_t, claimSequence, uint64_t, leaseDeadline, uint64_t,
                                    cutoffReserveSeconds)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, chunkserverSessionLease, SAU_MATOCS_CS_SESSION_LEASE, 0,
                                    uint32_t, stableId, uint64_t, chunkserverIncarnation, uint32_t,
                                    renewerMdsId, uint64_t, renewerMdsIncarnation, uint64_t,
                                    claimSequence, uint64_t, leaseDeadline, uint64_t,
                                    cutoffReserveSeconds)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, clusterMembers, SAU_MATOCS_CLUSTER_MEMBERS, 0, uint32_t,
                                    senderMdsId, std::vector<MetadataserverClusterEntry>, members)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, setVersion, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, setVersion, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, setVersion, SAU_MATOCS_SET_VERSION,
                                    kStandardAndXorChunks, uint64_t, chunkId, legacy::ChunkPartType,
                                    chunkType, uint32_t, chunkVersion, uint32_t, newVersion)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, setVersion, SAU_MATOCS_SET_VERSION, kECChunks, uint64_t,
                                    chunkId, ChunkPartType, chunkType, uint32_t, chunkVersion,
                                    uint32_t, newVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, setVersionAndLock, kECChunks, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, setVersionAndLock, SAU_MATOCS_SET_VERSION_AND_LOCK,
                                    kECChunks, uint64_t, chunkId, ChunkPartType, chunkType,
                                    uint32_t, chunkVersion, uint32_t, newVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, chunkLock, kECChunks, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, chunkLock, SAU_MATOCS_LOCK_CHUNK, kECChunks, uint64_t,
                                    chunkId, ChunkPartType, chunkType)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, chunkUnlock, kECChunks, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, chunkUnlock, SAU_MATOCS_UNLOCK_CHUNK, kECChunks,
                                    uint64_t, chunkId, ChunkPartType, chunkType)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, deleteChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, deleteChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, deleteChunk, SAU_MATOCS_DELETE_CHUNK,
                                    kStandardAndXorChunks, uint64_t, chunkId, legacy::ChunkPartType,
                                    chunkType, uint32_t, chunkVersion)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, deleteChunk, SAU_MATOCS_DELETE_CHUNK, kECChunks,
                                    uint64_t, chunkId, ChunkPartType, chunkType, uint32_t,
                                    chunkVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, createChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, createChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, createChunk, SAU_MATOCS_CREATE_CHUNK,
                                    kStandardAndXorChunks, uint64_t, chunkId, legacy::ChunkPartType,
                                    chunkType, uint32_t, chunkVersion)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, createChunk, SAU_MATOCS_CREATE_CHUNK, kECChunks,
                                    uint64_t, chunkId, ChunkPartType, chunkType, uint32_t,
                                    chunkVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, createAndLockChunk, kECChunks, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, createAndLockChunk, SAU_MATOCS_CREATE_AND_LOCK_CHUNK,
                                    kECChunks, uint64_t, chunkId, ChunkPartType, chunkType,
                                    uint32_t, chunkVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, truncateChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, truncateChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, truncateChunk, SAU_MATOCS_TRUNCATE,
                                    kStandardAndXorChunks, uint64_t, chunkId, legacy::ChunkPartType,
                                    chunkType, uint32_t,
                                    length,  // if xor chunk - length of chunk part
                                    uint32_t, newVersion, uint32_t, oldVersion)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, truncateChunk, SAU_MATOCS_TRUNCATE, kECChunks, uint64_t,
                                    chunkId, ChunkPartType, chunkType, uint32_t,
                                    length,  // if xor chunk - length of chunk part
                                    uint32_t, newVersion, uint32_t, oldVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, duplicateChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, duplicateChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, duplicateChunk, SAU_MATOCS_DUPLICATE_CHUNK,
                                    kStandardAndXorChunks, uint64_t, newChunkId, uint32_t,
                                    newchunkVersion, legacy::ChunkPartType, chunkType, uint64_t,
                                    oldChunkId, uint32_t, oldChunkVersion)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, duplicateChunk, SAU_MATOCS_DUPLICATE_CHUNK, kECChunks,
                                    uint64_t, newChunkId, uint32_t, newchunkVersion, ChunkPartType,
                                    chunkType, uint64_t, oldChunkId, uint32_t, oldChunkVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, duplicateAndLockChunk, kECChunks, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, duplicateAndLockChunk,
                                    SAU_MATOCS_DUPLICATE_AND_LOCK_CHUNK, kECChunks, uint64_t,
                                    newChunkId, uint32_t, newChunkVersion, ChunkPartType, chunkType,
                                    uint64_t, oldChunkId, uint32_t, oldChunkVersion)

SAUNAFS_DEFINE_PACKET_VERSION(matocs, duptruncChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, duptruncChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, duptruncChunk, SAU_MATOCS_DUPTRUNC_CHUNK,
                                    kStandardAndXorChunks, uint64_t, newChunkId, uint32_t,
                                    newchunkVersion, legacy::ChunkPartType, chunkType, uint64_t,
                                    oldChunkId, uint32_t, oldChunkVersion, uint32_t,
                                    length)  // if xor chunk - length of chunk part
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, duptruncChunk, SAU_MATOCS_DUPTRUNC_CHUNK, kECChunks,
                                    uint64_t, newChunkId, uint32_t, newchunkVersion, ChunkPartType,
                                    chunkType, uint64_t, oldChunkId, uint32_t, oldChunkVersion,
                                    uint32_t, length)  // if xor chunk - length of chunk part

SAUNAFS_DEFINE_PACKET_VERSION(matocs, replicateChunk, kStandardAndXorChunks, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocs, replicateChunk, kECChunks, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, replicateChunk, SAU_MATOCS_REPLICATE_CHUNK,
                                    kStandardAndXorChunks, uint64_t, chunkId, uint32_t,
                                    chunkVersion, legacy::ChunkPartType, chunkType,
                                    std::vector<legacy::ChunkTypeWithAddress>, sources)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, replicateChunk, SAU_MATOCS_REPLICATE_CHUNK, kECChunks,
                                    uint64_t, chunkId, uint32_t, chunkVersion, ChunkPartType,
                                    chunkType, std::vector<ChunkTypeWithAddress>, sources)

namespace matocs {
namespace replicateChunk {

inline void deserializePartial(const std::vector<uint8_t> &source, uint64_t &chunkId,
                               uint32_t &chunkVersion, legacy::ChunkPartType &chunkType,
                               const uint8_t *&sources) {
	verifyPacketVersionNoHeader(source, kStandardAndXorChunks);
	deserializeAllPacketDataNoHeader(source, chunkId, chunkVersion, chunkType, sources);
}

inline void deserializePartial(const std::vector<uint8_t> &source, uint64_t &chunkId,
                               uint32_t &chunkVersion, ChunkPartType &chunkType,
                               const uint8_t *&sources) {
	verifyPacketVersionNoHeader(source, kECChunks);
	deserializeAllPacketDataNoHeader(source, chunkId, chunkVersion, chunkType, sources);
}

}  // namespace replicateChunk
}  // namespace matocs

// Acknowledges the evidence outbox up to a sequence: sent only after the observations and the
// source's idempotency position were committed in one transaction, which is what entitles the
// source to forget them.
SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, evidenceAck, SAU_MATOCS_EVIDENCE_ACK, 0, uint64_t,
                                    ackedUpToSequence)

// The fenced chunk command plane. Each command carries the identity its reply must echo, ahead
// of the parameters it shares with the erasure coded legacy variant. There is one type per
// command rather than one kind switched command, which keeps the one type per command style the
// rest of this protocol already has, and it means an unknown command is rejected by the frame
// dispatcher instead of by a switch inside a handler that already accepted the frame.
//
// The `andLock` variants of the legacy plane are folded into a `needsLock` flag here. They exist
// on the legacy plane because a chunkserver may be too old to lock; every chunkserver on this
// plane is new enough by construction, so the distinction is a parameter rather than a type.

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, fencedCreateChunk, SAU_MATOCS_FENCED_CREATE_CHUNK, 0,
                                    ChunkCommandIdentity, identity, uint64_t, chunkId,
                                    ChunkPartType, chunkType, uint32_t, chunkVersion, bool,
                                    needsLock)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, fencedDeleteChunk, SAU_MATOCS_FENCED_DELETE_CHUNK, 0,
                                    ChunkCommandIdentity, identity, uint64_t, chunkId,
                                    ChunkPartType, chunkType, uint32_t, chunkVersion)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, fencedSetVersion, SAU_MATOCS_FENCED_SET_VERSION, 0,
                                    ChunkCommandIdentity, identity, uint64_t, chunkId,
                                    ChunkPartType, chunkType, uint32_t, chunkVersion, uint32_t,
                                    newVersion, bool, needsLock)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, fencedDuplicateChunk,
                                    SAU_MATOCS_FENCED_DUPLICATE_CHUNK, 0, ChunkCommandIdentity,
                                    identity, uint64_t, newChunkId, uint32_t, newChunkVersion,
                                    ChunkPartType, chunkType, uint64_t, oldChunkId, uint32_t,
                                    oldChunkVersion, bool, needsLock)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, fencedTruncateChunk, SAU_MATOCS_FENCED_TRUNCATE, 0,
                                    ChunkCommandIdentity, identity, uint64_t, chunkId,
                                    ChunkPartType, chunkType, uint32_t, length, uint32_t,
                                    newVersion, uint32_t, oldVersion)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, fencedDuptruncChunk, SAU_MATOCS_FENCED_DUPTRUNC_CHUNK,
                                    0, ChunkCommandIdentity, identity, uint64_t, newChunkId,
                                    uint32_t, newChunkVersion, ChunkPartType, chunkType, uint64_t,
                                    oldChunkId, uint32_t, oldChunkVersion, uint32_t, length)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, fencedReplicateChunk,
                                    SAU_MATOCS_FENCED_REPLICATE_CHUNK, 0, ChunkCommandIdentity,
                                    identity, uint64_t, chunkId, uint32_t, chunkVersion,
                                    ChunkPartType, chunkType, std::vector<ChunkTypeWithAddress>,
                                    sources)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, fencedVerifyPart, SAU_MATOCS_FENCED_VERIFY_PART, 0,
                                    ChunkCommandIdentity, identity, uint64_t, chunkId,
                                    ChunkPartType, chunkType)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, fencedLockChunk, SAU_MATOCS_FENCED_LOCK_CHUNK, 0,
                                    ChunkCommandIdentity, identity, uint64_t, chunkId,
                                    ChunkPartType, chunkType)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocs, fencedUnlockChunk, SAU_MATOCS_FENCED_UNLOCK_CHUNK, 0,
                                    ChunkCommandIdentity, identity, uint64_t, chunkId,
                                    ChunkPartType, chunkType)

namespace matocs {
namespace fencedReplicateChunk {

/// Stops at the source vector and hands back a pointer to it, so the replication job can read the
/// sources straight out of the received buffer the way the legacy path already does.
inline void deserializePartial(const std::vector<uint8_t> &source, ChunkCommandIdentity &identity,
                               uint64_t &chunkId, uint32_t &chunkVersion, ChunkPartType &chunkType,
                               const uint8_t *&sources) {
	verifyPacketVersionNoHeader(source, 0);
	deserializeAllPacketDataNoHeader(source, identity, chunkId, chunkVersion, chunkType, sources);
}

}  // namespace fencedReplicateChunk
}  // namespace matocs
