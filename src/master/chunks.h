/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

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
struct csdbentry;

inline Signal<uint64_t, uint32_t, uint32_t, uint32_t> gChunkChangedSignal;

/// Emitted with the chunk id when a chunk is dropped from the in-memory metadata, so KV backends
/// can delete its persisted row instead of resurrecting it on the next load.
inline Signal<uint64_t> gChunkRemovedSignal;

inline std::unique_ptr<IIdGeneratorWithState<uint64_t>> gChunkIdGenerator = nullptr;

void chunk_add_from_initial_metadata_load(uint64_t chunkId, uint32_t chunkVersion,
                                          uint32_t lockedTo, uint32_t lockId);
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

/// True when the chunk's copies changed since the last version operation completed, so the
/// next write must bump the version first. False for an unknown chunk.
bool chunk_needs_version_increase(uint64_t chunkid);

/// Returns true if the chunk is present in the in-memory hash.
bool chunk_exists(uint64_t chunkid);

/// True when the in-memory chunk knows at least one part, whatever its state. A chunk rebuilt
/// from durable metadata alone knows none.
bool chunk_has_parts(uint64_t chunkid);

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
/// One stored part named by its chunkserver, the currency between a backend that keeps
/// locations outside memory and the in-memory chunk table. The csdbentry outlives connections,
/// so a location stays valid across a reconnect; its eptr says whether the server is live.
struct ChunkPartLocation {
	csdbentry *server;
	ChunkPartType partType;
};

/// The two commands the maintenance planner issues to chunkservers.
enum class ChunkMaintenanceKind {
	kReplicate,
	kDelete
};

/// A planner decision handed to a backend's sink instead of being sent directly, so the backend
/// can record it before the chunkserver acts on it.
struct ChunkMaintenanceCommand {
	ChunkMaintenanceKind kind;
	uint64_t chunkid;
	/// For kReplicate the version the new copy is created at; for kDelete the exact version to
	/// delete, or 0 to delete whatever the server holds.
	uint32_t version;
	/// Server that receives the command.
	ChunkPartLocation destination;
	/// Copies to replicate from; empty for kDelete.
	std::vector<ChunkPartLocation> sources;
};

/// Receives each planner command synchronously. Returns true when the command was sent, false
/// to refuse it; a refused command is simply not issued and the planner revisits the chunk later.
using ChunkMaintenanceSink = std::function<bool(const ChunkMaintenanceCommand &)>;

/// Runs the existing planner on a disposable snapshot of one chunk, without caching it, so a
/// backend that pages its own records can reuse Master's placement rules. The sink sends the
/// accepted commands; the snapshot retains nothing. Returns EINVAL for an inconsistent snapshot,
/// LOCKED or CHUNKBUSY when a cached copy of the chunk is in use.
int chunk_run_maintenance(uint64_t chunkid, uint32_t version,
                          const std::vector<ChunkPartLocation> &parts,
                          const std::vector<ChunkGoalCounters::GoalCounter> &goals,
                          const ChunkMaintenanceSink &sink);

/// Refills an idle cached chunk from the caller's authoritative snapshot, older versions
/// included, without sending commands or mutation notifications; the caller's records, not this
/// process, decide what the chunk holds. Validates parts and goal counts before mutating.
int chunk_replace_part_locations(uint64_t chunkid, uint32_t version, uint32_t lockid,
                                 uint32_t lockedto, const std::vector<ChunkPartLocation> &parts,
                                 size_t maxParts,
                                 const std::vector<ChunkGoalCounters::GoalCounter> &goals,
                                 bool needVersionIncrease);

/// Readable current-version parts of a chunk a client just unlocked, the set a backend publishes
/// at write end. Excludes parts with an unfinished command; retiring copies and buffered writes
/// stay readable as they always were. An error leaves the output empty.
int chunk_get_publishable_parts(uint64_t chunkid, uint32_t &version,
                                std::vector<ChunkPartLocation> &parts, size_t maxParts);

/// Live parts stored at @p version whatever their validity state, for a repair or a settled
/// operation that adopts that version. Parts with an unfinished command are excluded; retiring
/// copies are included, as in chunk_get_publishable_parts.
int chunk_get_parts_at_version(uint64_t chunkid, uint32_t version,
                               std::vector<ChunkPartLocation> &parts, size_t maxParts);

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

enum class ChunkRepairAction : uint8_t {
	kUnchanged = 0,
	kEraseReference = 1,
	kSetVersion = 2,
};

struct ChunkRepairPlan {
	ChunkRepairAction action = ChunkRepairAction::kUnchanged;
	uint32_t version = 0;
};

/// Computes repair work without changing the in-memory chunk registry.
ChunkRepairPlan chunk_plan_repair(uint64_t ochunkid, uint8_t correct_only);

/// Applies an exact plan produced by chunk_plan_repair().
bool chunk_apply_repair_plan(uint8_t goal, uint64_t ochunkid, const ChunkRepairPlan &plan);

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

/// Enables or disables background chunk maintenance commands (repair, replication, physical
/// deletion, rebalancing) sent to chunkservers. Must be called before promotion; the default is
/// enabled. The chunk worker keeps running either way: it still refreshes each chunk's cached
/// statistics, which client write and truncate paths rely on, and still drops already-empty
/// in-memory chunk records. Foreground client operations and their recovery stay enabled; only
/// the listed background maintenance commands are suppressed.
void chunk_set_maintenance_enabled(bool enabled);
/// Deletes the copies of in-memory chunks that no file references and that have no durable
/// record (a create or copy-on-write attempt that never committed). Master's own loop does this
/// in place; a backend that pages records instead calls it with a per-tick budget. Locked or busy
/// chunks wait. Returns how many chunks were visited.
size_t chunk_reclaim_unreferenced(size_t budget, const std::function<bool(uint64_t)> &hasRecord);
/// True when memory holds a valid part of @p chunkid on @p server at the chunk's version, so a
/// backend can skip re-verifying a copy this process itself created after the server returned.
bool chunk_server_holds_valid_part(uint64_t chunkid, matocsserventry *server);

/// Ticks of chunk_jobs_main that one pass over the in-memory chunk table takes, derived from the
/// hash size and the configured steps per tick; a backend paging its own records paces itself
/// the same way so per-server command limits behave as under Master.
uint32_t chunk_maintenance_ticks_per_pass();
/// Time budget of one maintenance tick in milliseconds, the configured loop timeout.
uint32_t chunk_maintenance_tick_budget_ms();

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
