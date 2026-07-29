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
#include "common/chunk_with_version_and_type.h"
#include "common/chunks_availability_state.h"
#include "common/media_label.h"
#include "master/checksum.h"

struct matocsserventry;
class FilesystemOperationContext;

/// Operations over chunk metadata, behind a swappable/extensible backend.
///
/// Layering mirrors the FilesystemOperations family, with the InMemory layer
/// that family is missing:
///   IChunkOperations  -- this interface (the external surface)
///   ChunkOperationsBase -- default behavior: forwards to the in-memory engine
///                          in chunks.{h,cc}. Will evolve into shared logic for multiple backends.
///   ChunkOperationsInMemory -- empty leaf over Base (leil-master). Will evolve into in-memory
///                              specific logic.
///   ChunkOperationsKV       -- overrides only the methods that persist to a KV store
///
/// A backend overrides only the operations it needs to specialize; everything
/// else falls through to the in-memory default, so the seam can be extended
/// incrementally without touching call sites.
class IChunkOperations {
public:
	/// Default constructor
	IChunkOperations() = default;

	/// Unneeded copy/assign constructors/operators
	IChunkOperations(const IChunkOperations &) = delete;
	IChunkOperations &operator=(const IChunkOperations &) = delete;
	IChunkOperations(IChunkOperations &&) = delete;
	IChunkOperations &operator=(IChunkOperations &&) = delete;

	/// Virtual destructor
	virtual ~IChunkOperations() = default;

	// --- Reference counting / goal (persisted refcount) ---
	// fsOpContext carries the KV transaction so the refcount write joins the same
	// transaction as the triggering node mutation; the in-memory backend ignores it.
	virtual int addFile(const FilesystemOperationContext &fsOpContext, uint64_t chunkid,
	                    uint8_t goal, bool isMetadataLoading = false) = 0;
	virtual int deleteFile(const FilesystemOperationContext &fsOpContext, uint64_t chunkid,
	                       uint8_t goal) = 0;
	virtual int changeFile(const FilesystemOperationContext &fsOpContext, uint64_t chunkid,
	                       uint8_t prevGoal, uint8_t newGoal) = 0;
	/// Whether successful add/delete/change calls are only staged for the transaction.
	/// Generic callers use this to avoid compensating changes that never reached the
	/// in-memory registry.
	virtual bool defersFileReferenceMutations(
	    const FilesystemOperationContext &fsOpContext) const = 0;

	// --- Version (persisted version) ---
	virtual int increaseVersion(const FilesystemOperationContext &fsOpContext,
	                            uint64_t chunkid) = 0;
	virtual int setVersion(const FilesystemOperationContext &fsOpContext, uint64_t chunkid,
	                       uint32_t version) = 0;
	virtual void persistRecord(const FilesystemOperationContext &fsOpContext, uint64_t chunkid) = 0;

	// --- Locking ---
	virtual int unlock(uint64_t chunkid) = 0;

	// --- Id-allocation watermark.
	// KV stub: the KV backend owns this via META_NEXT_CHUNK_RANGE (non-monotonic
	// generator), so chunk_server_has_chunk never bumps it. ---
	virtual uint8_t setNextChunkId(uint64_t nextChunkIdToBeSet) = 0;

	// --- Changelog replay.
	// KV stub: reached only from fs_apply_* replay, which the KV backend does not
	// run (the KV store is the source of truth). ---
	virtual uint8_t applyModification(uint32_t ts, uint64_t oldChunkId, uint32_t lockid,
	                                  uint8_t goal, bool doIncreaseVersion,
	                                  uint64_t *newChunkId) = 0;

#ifndef METARESTORE
	// --- Write / modify (client write path) ---
	virtual uint8_t multiModify(const FilesystemOperationContext &fsOpContext, uint64_t ochunkid,
	                            uint32_t *lockid, uint8_t goal, bool quotaExceeded, uint8_t *opflag,
	                            uint64_t *nchunkid, uint32_t minServerVersion) = 0;
	virtual uint8_t multiTruncate(const FilesystemOperationContext &fsOpContext, uint64_t ochunkid,
	                              uint32_t lockid, uint32_t length, uint8_t goal,
	                              bool denyTruncatingParityParts, bool quotaExceeded,
	                              uint64_t *nchunkid) = 0;
	virtual int canUnlock(uint64_t chunkid, uint32_t lockid) = 0;

	// --- Read path (locations) ---
	virtual int getVersionAndLocations(uint64_t chunkid, uint32_t currentIp, uint32_t &version,
	                                   uint32_t maxNumberOfChunkCopies,
	                                   std::vector<ChunkTypeWithAddress> &serversList) = 0;
	virtual int getVersionAndLocations(uint64_t chunkid, uint32_t currentIp, uint32_t &version,
	                                   uint32_t maxNumberOfChunkCopies,
	                                   std::vector<ChunkPartWithAddressAndLabel> &serversList) = 0;

	// --- Repair / health queries ---
	virtual int repair(const FilesystemOperationContext &fsOpContext, uint8_t goal,
	                   uint64_t ochunkid, uint32_t *nversion, uint8_t correctOnly) = 0;
	virtual int getFullCopies(uint64_t chunkid, uint8_t *vcopies) = 0;
	virtual int getPartsToModify(uint64_t chunkid, int &recover, int &remove) = 0;
	virtual bool hasOnlyInvalidCopies(uint64_t chunkid) = 0;

	// --- Chunkserver session ingestion ---
	virtual void serverHasChunk(matocsserventry *ptr, uint64_t chunkid,
	                            uint32_t versionWithTodelFlag, ChunkPartType chunkType) = 0;
	virtual void serverHasChunks(matocsserventry *ptr,
	                             const std::vector<ChunkWithVersionAndType> &chunks) = 0;
	virtual void damaged(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunkType) = 0;
	virtual void lost(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunkType) = 0;
	virtual void serverDisconnected(matocsserventry *ptr, const MediaLabel &label) = 0;
	virtual void serverUnlabelledConnected() = 0;
	virtual void serverLabelChanged(const MediaLabel &previousLabel,
	                                const MediaLabel &newLabel) = 0;

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
	virtual void gotSetVersionStatus(matocsserventry *ptr, uint64_t chunkId,
	                                 ChunkPartType chunkType, uint8_t status) = 0;
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
	// KV stub: checksums belong to the Master/Shadow dump model, not the KV store. ---
	virtual uint64_t checksum(ChecksumMode mode) = 0;
	virtual ChecksumRecalculationStatus updateChecksumABit(uint32_t speedLimit) = 0;
};

/// The active chunk-operations backend, bound at startup (InMemory for
/// leil-master, KV for the KV-backed build), mirroring gFSOperations.
inline std::unique_ptr<IChunkOperations> gChunkOperations = nullptr;
