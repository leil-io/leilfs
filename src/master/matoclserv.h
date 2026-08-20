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

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/type_defs.h"

class FilesystemOperationContext;

/// Get stats from client and reset them in the server.
/// @param stats Array of 5 elements to store stats in the following order:
/// 0 - packets received
/// 1 - packets sent
/// 2 - bytes received
/// 3 - bytes sent
void matoclserv_stats(uint64_t stats[5]);

/// Sends the status of a delayed operation associated with a chunk over the network.
/// @param chunkId The ID of the chunk associated with the delayed operation
/// @param status  The status of the operation, (e.g., SAUNAFS_STATUS_OK, SAUNAFS_ERROR_NOTDONE)
/// @param isFailedCreateOperation True if the operation was a failed create operation, false
/// otherwise
void matoclserv_chunk_status(uint64_t chunkId, uint8_t status,
                             bool isFailedCreateOperation = false);

/// Notifies all clients waiting for a given chunk ID to be unlocked.
/// @param chunkId The ID of the chunk that has been unlocked
void matoclserv_notify_unlock_list(uint64_t chunkId);

/// Reissues a staged KV changelog version into the strictly increasing published stream.
/// In-memory master bypasses this; its inline versions are already serialized.
uint64_t matoclserv_sequence_published_changelog_version(uint64_t stagedVersion);

/// Fans one formatted changelog entry out to every sink: the changelog file/replay, the
/// changelog signal, metaloggers, and notifiers. `entry` must be NUL-terminated for the
/// C-string sinks; `size` counts the trailing NUL so broadcasts match the inline framing.
void matoclserv_emit_changelog_sinks(uint64_t version, char *entry, std::size_t size);

/// Publishes and drains changelog entries buffered by a transaction whose commit is known
/// durable. The context is marked committed before publication so an undrained buffer trips its
/// destructor invariant.
void matoclserv_publish_committed_changelog(const FilesystemOperationContext &context);

/// Initializes the network configuration and register the eventloop callbacks.
/// @return 0 on success, negative value on error
int matoclserv_network_init();

/// Notify interested clients about the status of metadata saving process.
/// @param status Status of the metadata saving process
void matoclserv_broadcast_metadata_saved(uint8_t status);

/// Notify interested clients about the status of metadata checksum recalculation process.
/// @param status Status of the metadata checksum recalculation process
void matoclserv_broadcast_metadata_checksum_recalculated(uint8_t status);

/// Check whether there are any async filesystem operations that still need to finished (e.g delayed
/// chunk operations)
bool matoclserv_client_async_operations_finished();

/// Group-commit runtime counters, for diagnosing how well batching amortizes
/// durability under a given load. committedOps/committedBatches is the average ops per
/// commit (the headline number: near 1 means batching is not engaging, latency-bound).
/// Counted on the single event-loop thread; gathered only while batch-stats accounting is
/// enabled (see matoclserv_set_batch_stats_enabled).
struct MatoclBatchStats {
	uint64_t committedBatches = 0;  ///< batches whose single commit became durable
	uint64_t committedOps = 0;      ///< total member ops across those batches
	uint64_t maxBatchSize = 0;      ///< largest committed batch
	uint64_t batchReplays = 0;      ///< whole-batch conflict replays (external writer)
	uint64_t heldOps = 0;           ///< ops parked because a batch commit was in flight
	uint64_t sizeBuckets[5] = {0};  ///< committed-batch size histogram: 1, 2-3, 4-7, 8-15, 16+
};

/// Enables/disables group-commit batch-stats accounting (default off; the master turns it on
/// with FDB_OP_PROFILING). When off, the hot-path counters are a single bool check.
void matoclserv_set_batch_stats_enabled(bool enabled);

/// Returns the current group-commit batch-stats counters (process-wide, since process start).
MatoclBatchStats matoclserv_get_batch_stats();

/// Returns the port this instance is actually listening on for client/admin connections,
/// resolved the same way the listener resolved it at bind time (a number or a service
/// name); 0 means no concrete port (the ephemeral "*", or a string naming no port).
/// Can differ from MATOCL_LISTEN_PORT's raw config value right after a reload that failed
/// to rebind: the listener then keeps its previous, working port, but config already shows
/// the new one.
uint16_t matoclserv_get_listen_port();

/// Returns the host this instance is actually bound to for client/admin connections. Can
/// differ from MATOCL_LISTEN_HOST's raw config value the same way matoclserv_get_listen_port
/// can differ from its own config value.
const std::string &matoclserv_get_listen_host();

/// Requests one quiescent group-commit window for background metadata maintenance.
/// Already staged client work is committed first; newly submitted bodies remain held
/// and unexecuted until matoclserv_complete_commit_pipeline_maintenance is called.
void matoclserv_request_commit_pipeline_maintenance();

/// Ends a previously requested maintenance window and lets held client operations resume.
void matoclserv_complete_commit_pipeline_maintenance();
