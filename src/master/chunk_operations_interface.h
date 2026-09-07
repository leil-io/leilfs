/*
   Copyright 2026      Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "common/chunk_health_freshness.h"
#include "common/chunk_part_type.h"
#include "common/chunk_type_with_address.h"
#include "common/chunk_with_address_and_label.h"
#include "common/chunk_with_version_and_type.h"
#include "common/chunks_availability_state.h"
#include "common/chunkserver_id.h"
#include "common/media_label.h"
#include "common/network_address.h"
#include "master/checksum.h"
#include "protocol/chunks_with_type.h"

struct matocsserventry;
class FilesystemOperationContext;

/// What a chunkserver announced by the time its registration completed, passed whole to
/// serverRegistered so the seam signature survives additions to the registration packets.
struct ChunkserverRegistration {
	/// Address clients and other chunkservers use to reach this server.
	NetworkAddress endpoint;
	/// Software version; the identity exchange needs kFirstVersionWithChunkserverIdentity.
	uint32_t version;
	/// Keepalive timeout the chunkserver requested for this connection, in milliseconds.
	uint32_t timeoutMs;
	/// Persistent identity; absent when the backend did not ask or the peer predates it.
	std::optional<chunkserver::ChunkserverId> chunkserverId;
};

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

	/// Locations for a client that holds the write lock. A backend that hides a chunk's readable
	/// locations for the duration of a write still answers the writer from its own state.
	virtual int getWriteVersionAndLocations(uint64_t chunkid, uint32_t currentIp, uint32_t &version,
	                                        uint32_t maxNumberOfChunkCopies,
	                                        std::vector<ChunkTypeWithAddress> &serversList) = 0;

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
	/// A whole damaged-report packet, so a backend can apply it in one transaction; the base
	/// applies the single-chunk handler to each entry in order.
	virtual void damagedChunks(matocsserventry *ptr, const std::vector<ChunkWithType> &chunks) = 0;
	/// A whole lost-report packet, with the same contract as damagedChunks.
	virtual void lostChunks(matocsserventry *ptr, const std::vector<ChunkWithType> &chunks) = 0;
	/// Whether registration asks the chunkserver for its persistent identity before completing.
	/// A backend that records chunk locations by chunkserver identity needs it; the in-memory
	/// registry keys copies by connection and does not.
	virtual bool requestsChunkserverIdentity() const = 0;
	/// Whether the backend feeds the maintenance loop bounded snapshots of its own records
	/// instead of letting it walk the cached chunk table; the loop then drives maintenanceTick
	/// and maintenanceStep.
	virtual bool usesExternalMaintenance() const = 0;
	/// Arms one tick of external maintenance work; maintenanceStep spends it in slices.
	virtual void maintenanceTick() = 0;
	/// One slice of the current tick's work, called between polls; true when more slices remain.
	virtual bool maintenanceStep() = 0;
	/// The chunkserver's registration completed, identity included when it was requested; a
	/// backend without a registration inventory restores what it knows about the server here.
	virtual void serverRegistered(matocsserventry *ptr,
	                              const ChunkserverRegistration &registration) = 0;
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
	/// Reply to a chunk probe: the version the chunkserver holds, or a NOCHUNK status. A backend
	/// without a registration inventory asks instead of trusting what it last recorded.
	virtual void gotProbeStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                            uint32_t chunkVersion, uint8_t status) = 0;
	/// Every part answered the chunk's pending operation and the chunk is idle again. A backend
	/// that keeps the part set outside memory publishes it here.
	virtual void operationSettled(uint64_t chunkId) = 0;
	/// A part of @p chunkId held by the disconnected chunkserver @p csid left memory. A backend
	/// without a registration inventory restores it from its own records when the server returns.
	virtual void copyDropped(uint64_t chunkId, uint16_t csid) = 0;

	// --- Stats / introspection ---
	virtual void stats(uint32_t *del, uint32_t *repl) = 0;
	virtual uint32_t getChunkInfoSerializedSize() = 0;
	virtual void storeChunkInfo(uint8_t *buff) = 0;
	virtual uint32_t getMissingCount() = 0;
	virtual void storeChunkCounters(uint8_t *buff, uint8_t matrixid) = 0;
	virtual uint32_t count() = 0;
	virtual const ChunksReplicationState &getReplicationState() = 0;
	virtual const ChunksAvailabilityState &getAvailabilityState() = 0;
	/// When the two states above were measured, for a backend that derives them from its records
	/// instead of maintaining them as chunks change. Empty means the counters are current by
	/// construction, so there is no measurement to date.
	virtual std::optional<ChunkHealthFreshness> getHealthFreshness() = 0;
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
