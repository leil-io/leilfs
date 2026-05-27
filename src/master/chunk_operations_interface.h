/*
   Copyright 2026      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "common/chunk_part_type.h"
#include "common/chunk_type_with_address.h"
#include "common/chunk_with_address_and_label.h"
#include "common/chunks_availability_state.h"
#include "common/media_label.h"
#include "master/checksum.h"

struct matocsserventry;

/// Operations over chunk metadata, behind a swappable backend.
///
/// Layering mirrors the FilesystemOperations family, with the InMemory layer
/// that family is missing:
///   IChunkOperations  -- this interface (the external surface)
///   ChunkOperationsBase -- default behavior: forwards to the in-memory engine
///                          in chunks.{h,cc}
///   ChunkOperationsInMemory -- empty leaf over Base (leil-master)
///   ChunkOperationsKV       -- overrides only the methods that persist to a KV
///                              store like FoundationDB (leil-mds)
///
/// In Step 1 only the refcount/version deltas, the startup rebuild, and the
/// chunkserver-report lookup persist; KV inherits the rest (locations, status
/// callbacks, maintenance) as in-memory while the location layer still lives in
/// RAM. The protected storage primitives below become the InMemory/KV divergence
/// point once that layer moves to FDB.
class IChunkOperations {
public:
	virtual ~IChunkOperations() = default;

	// --- Reference counting / goal (the persisted refcount, Step 1) ---
	virtual int addFile(uint64_t chunkid, uint8_t goal, bool isMetadataLoading = false) = 0;
	virtual int deleteFile(uint64_t chunkid, uint8_t goal) = 0;
	virtual int changeFile(uint64_t chunkid, uint8_t prevGoal, uint8_t newGoal) = 0;

	// --- Version (the persisted version, Step 1) ---
	virtual int increaseVersion(uint64_t chunkid) = 0;
	virtual int setVersion(uint64_t chunkid, uint32_t version) = 0;

	// --- Locking ---
	virtual int unlock(uint64_t chunkid) = 0;

	// --- Id-allocation watermark.
	// KV stub: the FDB MDS owns this via META_NEXT_CHUNK_RANGE (non-monotonic
	// generator), so chunk_server_has_chunk never bumps it. ---
	virtual uint8_t setNextChunkId(uint64_t nextChunkIdToBeSet) = 0;

	// --- Changelog replay.
	// KV stub: reached only from fs_apply_* replay, which the FDB MDS does not
	// run (FoundationDB is the source of truth). ---
	virtual uint8_t applyModification(uint32_t ts, uint64_t oldChunkId, uint32_t lockid,
	                                  uint8_t goal, bool doIncreaseVersion,
	                                  uint64_t *newChunkId) = 0;

#ifndef METARESTORE
	// --- Write / modify (client write path) ---
	virtual uint8_t multiModify(uint64_t ochunkid, uint32_t *lockid, uint8_t goal,
	                            bool quotaExceeded, uint8_t *opflag, uint64_t *nchunkid,
	                            uint32_t minServerVersion) = 0;
	virtual uint8_t multiTruncate(uint64_t ochunkid, uint32_t lockid, uint32_t length,
	                              uint8_t goal, bool denyTruncatingParityParts,
	                              bool quotaExceeded, uint64_t *nchunkid) = 0;
	virtual int canUnlock(uint64_t chunkid, uint32_t lockid) = 0;

	// --- Read path (locations) ---
	virtual int getVersionAndLocations(uint64_t chunkid, uint32_t currentIp, uint32_t &version,
	                                   uint32_t maxNumberOfChunkCopies,
	                                   std::vector<ChunkTypeWithAddress> &serversList) = 0;
	virtual int getVersionAndLocations(uint64_t chunkid, uint32_t currentIp, uint32_t &version,
	                                   uint32_t maxNumberOfChunkCopies,
	                                   std::vector<ChunkPartWithAddressAndLabel> &serversList) = 0;

	// --- Repair / health queries ---
	virtual int repair(uint8_t goal, uint64_t ochunkid, uint32_t *nversion, uint8_t correctOnly) = 0;
	virtual int getFullCopies(uint64_t chunkid, uint8_t *vcopies) = 0;
	virtual int getPartsToModify(uint64_t chunkid, int &recover, int &remove) = 0;
	virtual bool hasOnlyInvalidCopies(uint64_t chunkid) = 0;

	// --- Chunkserver session ingestion ---
	virtual void serverHasChunk(matocsserventry *ptr, uint64_t chunkid,
	                            uint32_t versionWithTodelFlag, ChunkPartType chunkType) = 0;
	virtual void damaged(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunkType) = 0;
	virtual void lost(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunkType) = 0;
	virtual void serverDisconnected(matocsserventry *ptr, const MediaLabel &label) = 0;
	virtual void serverUnlabelledConnected() = 0;
	virtual void serverLabelChanged(const MediaLabel &previousLabel, const MediaLabel &newLabel) = 0;

	// --- Async operation results from chunkservers ---
	virtual void gotDeleteStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                             uint8_t status) = 0;
	virtual void gotReplicateStatus(matocsserventry *ptr, uint64_t chunkId, uint32_t chunkVersion,
	                                ChunkPartType chunkType, uint8_t status) = 0;
	virtual void gotCreateStatus(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunkType,
	                             uint8_t status) = 0;
	virtual void gotDuplicateStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                                uint8_t status) = 0;
	virtual void gotChunkLockStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                                uint8_t status) = 0;
	virtual void gotWriteEndStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                               uint8_t status) = 0;
	virtual void gotSetVersionStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                                 uint8_t status) = 0;
	virtual void gotTruncateStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                               uint8_t status) = 0;
	virtual void gotDuptruncStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                               uint8_t status) = 0;

	// --- Stats / introspection ---
	virtual void stats(uint32_t *del, uint32_t *repl) = 0;
	virtual uint32_t getChunkInfoSerializedSize() = 0;
	virtual void storeChunkInfo(uint8_t *buff) = 0;
	virtual uint32_t getMissingCount() = 0;
	virtual void storeChunkCounters(uint8_t *buff, uint8_t matrixid) = 0;
	virtual uint32_t count() = 0;
	virtual const ChunksReplicationState &getReplicationState() = 0;
	virtual const ChunksAvailabilityState &getAvailabilityState() = 0;
	virtual void info(uint32_t *allChunks, uint32_t *allCopies, uint32_t *regCopies) = 0;
	virtual int invalidateGoalCache() = 0;
#endif  // METARESTORE

	// --- Lifecycle ---
	virtual void newfs() = 0;
	virtual void unload() = 0;
	virtual int strinit() = 0;

	// --- Checksums.
	// KV stub: checksums belong to the Master/Shadow dump model, not FDB. ---
	virtual uint64_t checksum(ChecksumMode mode) = 0;
	virtual ChecksumRecalculationStatus updateChecksumABit(uint32_t speedLimit) = 0;

protected:
	// --- Storage primitives: the actual InMemory vs KV divergence point.
	//
	// Step 1 diverges ONLY on the chunk record (version + per-goal refcount):
	//   InMemory -> gChunksMetadata; KV -> CHNK_<chunkid> in FoundationDB.
	// Locations, chunk_got_*_status, getVersionAndLocations and the maintenance
	// sweep stay shared in-memory this step and become primitives only when the
	// location layer moves to FDB.
	//
	// This set is intentionally minimal and grows as ChunkOperationsBase is
	// refactored to route record access through it (boundary discipline).
	//
	//   virtual bool      recordFind(uint64_t chunkid, ChunkRecord &out) = 0;
	//   virtual void      recordCreate(uint64_t chunkid, uint32_t version) = 0;
	//   virtual void      recordErase(uint64_t chunkid) = 0;
	//   virtual void      recordAddRef(uint64_t chunkid, uint8_t goal) = 0;
	//   virtual void      recordRemoveRef(uint64_t chunkid, uint8_t goal) = 0;
	//   virtual void      recordChangeGoal(uint64_t chunkid, uint8_t prev, uint8_t next) = 0;
	//   virtual uint32_t  recordVersion(uint64_t chunkid) = 0;
	//   virtual void      recordSetVersion(uint64_t chunkid, uint32_t version) = 0;
};

/// The active chunk-operations backend, bound at startup (InMemory for
/// leil-master, KV for leil-mds), mirroring gFSOperations.
inline std::unique_ptr<IChunkOperations> gChunkOperations = nullptr;
