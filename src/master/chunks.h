/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <cstdio>
#include <memory>

#include "common/chunk_part_type.h"
#include "common/chunk_type_with_address.h"
#include "common/chunk_with_address_and_label.h"
#include "common/chunks_availability_state.h"
#include "common/observable_property.h"
#include "master/checksum.h"
#include "master/chunk_goal_counters.h"
#include "master/id_generator_interface.h"
#include "master/metadata_loader.h"

struct matocsserventry;

inline Signal<uint64_t, uint32_t, uint32_t, uint32_t> gChunkChangedSignal;

inline std::unique_ptr<IIdGeneratorWithState<uint64_t>> gChunkIdGenerator = nullptr;

void chunk_add_from_initial_metadata_load(uint64_t chunkId, uint32_t chunkVersion,
                                          uint32_t lockedTo, uint32_t lockId);

/// Restores or updates a chunk entry during shadow sync paths with checkpoint undo.
///
/// Intended for shadow sync paths that replay undo information for chunks.
/// If the chunk is missing, it is created (this can happen when an earlier undo removed it and a
/// later undo re-adds it).
///
/// Side effects:
/// - Creates the chunk if missing (via chunk_add_from_initial_metadata_load()).
/// - Updates the in-memory chunk fields (version/lock state).
/// - Updates chunk checksum accounting (via chunk_update_checksum()).
///
/// @param chunkId Chunk identifier.
/// @param version Chunk version to set.
/// @param lockedTo Chunk lock expiration time (same semantics as in metadata).
/// @param lockId Chunk lock identifier.
/// @return SAUNAFS_STATUS_OK (no-op or success).
int chunk_restore_set(uint64_t chunkId, uint32_t chunkVersion, uint32_t lockedTo, uint32_t lockId);

/// Removes a chunk entry during shadow sync paths with checkpoint undo.
///
/// Intended for shadow sync paths that replay undo information for chunks.
/// If the chunk exists, it is unlinked from the internal chunk hash table, removed from checksum
/// accounting (including the recalculated checksum when applicable), and freed (via chunk_delete()).
/// If the chunk does not exist, this is a no-op.
///
/// @param chunkId Chunk identifier.
/// @return SAUNAFS_STATUS_OK (no-op or success).
int chunk_restore_remove(uint64_t chunkId);

int chunk_increase_version(uint64_t chunkid);
int chunk_set_version(uint64_t chunkid,uint32_t version);
int chunk_change_file(uint64_t chunkid,uint8_t prevgoal,uint8_t newgoal);
int chunk_delete_file(uint64_t chunkid,uint8_t goal);
int chunk_add_file(uint64_t chunkid, uint8_t goal, bool isMetadataLoading = false);
int chunk_unlock(uint64_t chunkid);

/// Reads a chunk's version and a snapshot of its per-goal reference counts.
/// Returns false if the chunk is unknown.
bool chunk_get_version_and_goal_counters(uint64_t chunkid, uint32_t &version,
                                         ChunkGoalCounters &counters);

/// Reads a chunk's write-lock state (lockid and lock expiry timestamp).
/// Returns false if the chunk is unknown.
bool chunk_get_lock_state(uint64_t chunkid, uint32_t &lockid, uint32_t &lockedto);

/// Returns true if the chunk is present in the in-memory hash.
bool chunk_exists(uint64_t chunkid);

/// Restores an in-memory chunk from persisted version, refs and lock state.
/// Used by on-demand restore paths when a chunk is not present in memory yet.
void chunk_create_with_goal_counters(uint64_t chunkid, uint32_t version,
                                     const std::vector<ChunkGoalCounters::GoalCounter> &goals,
                                     uint32_t lockid, uint32_t lockedto);

uint8_t chunk_apply_modification(uint32_t ts, uint64_t oldChunkId, uint32_t lockid, uint8_t goal,
		bool doIncreaseVersion, uint64_t *newChunkId);

bool should_increase_chunk_version_on_modification(uint8_t operation);

// Tries to set next chunk id to a passed value, returns status
uint8_t chunk_set_next_chunkid(uint64_t nextChunkIdToBeSet);

#ifdef METARESTORE
void chunk_dump(void);
#else
uint8_t chunk_multi_modify(uint64_t ochunkid, uint32_t *lockid, uint8_t goal, bool quota_exceeded,
                           uint8_t *opflag, uint64_t *nchunkid, uint32_t min_server_version);
uint8_t chunk_multi_truncate(uint64_t ochunkid, uint32_t lockid, uint32_t length,
		uint8_t goal, bool denyTruncatingParityParts, bool quota_exceeded, uint64_t *nchunkid);
void chunk_stats(uint32_t *del,uint32_t *repl);
uint32_t get_chunk_info_serialized_size();
void chunk_store_info(uint8_t *buff);
uint32_t chunk_get_missing_count(void);
void chunk_store_chunkcounters(uint8_t *buff,uint8_t matrixid);
uint32_t chunk_count(void);
const ChunksReplicationState& chunk_get_replication_state();
const ChunksAvailabilityState& chunk_get_availability_state();
void chunk_info(uint32_t *allchunks,uint32_t *allcopies,uint32_t *regcopies);

/// Checks if the given chunk has only invalid copies (ie. needs to be repaired).
bool chunk_has_only_invalid_copies(uint64_t chunkid);

int chunk_get_fullcopies(uint64_t chunkid,uint8_t *vcopies);
int chunk_get_partstomodify(uint64_t chunkid, int &recover, int &remove);
int chunk_repair(uint8_t goal,uint64_t ochunkid,uint32_t *nversion, uint8_t correct_only);

int chunk_getversionandlocations(uint64_t chunkid, uint32_t currentIp, uint32_t& version,
		uint32_t maxNumberOfChunkCopies, std::vector<ChunkTypeWithAddress>& serversList);
int chunk_getversionandlocations(uint64_t chunkid, uint32_t currentIp, uint32_t& version,
		uint32_t maxNumberOfChunkCopies, std::vector<ChunkPartWithAddressAndLabel>& serversList);
void chunk_server_has_chunk(matocsserventry *ptr, uint64_t chunkid, uint32_t versionWithTodelFlag, ChunkPartType chunkType);
void chunk_damaged(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunk_type);
void chunk_lost(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunk_type);
void chunk_server_disconnected(matocsserventry *ptr, const MediaLabel &label);
void chunk_server_unlabelled_connected();
void chunk_server_label_changed(const MediaLabel &previousLabel, const MediaLabel &newLabel);

void chunk_got_delete_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType, uint8_t status);
void chunk_got_replicate_status(matocsserventry *ptr, uint64_t chunkId, uint32_t chunkVersion,
		ChunkPartType chunkType, uint8_t status);

void chunk_got_create_status(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunkType, uint8_t status);
void chunk_got_duplicate_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType, uint8_t status);
void chunk_got_chunklock_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
                                uint8_t status);
void chunk_got_writeend_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
                               uint8_t status);
void chunk_got_setversion_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType, uint8_t status);
void chunk_got_truncate_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType, uint8_t status);
void chunk_got_duptrunc_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType, uint8_t status);

int chunk_can_unlock(uint64_t chunkid, uint32_t lockid);

int chunk_invalidate_goal_cache();

#endif

bool chunksLoadFromFile(MetadataLoader::Options);
void chunk_store(FILE *fd);
void chunk_unload(void);
void chunk_newfs(void);
int chunk_strinit(void);
/// Returns the next chunk ID to be used.
uint64_t chunk_get_next_id(void);
uint64_t chunk_checksum(ChecksumMode mode);
ChecksumRecalculationStatus chunks_update_checksum_a_bit(uint32_t speedLimit);
