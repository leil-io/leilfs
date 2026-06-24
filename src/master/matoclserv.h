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

#include "common/type_defs.h"

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

/// Initializes the network configuration and register the eventloop callbacks.
/// @return 0 on success, negative value on error
int matoclserv_network_init();

/// Notify interested clients about the status of metadata saving process.
/// @param status Status of the metadata saving process
void matoclserv_broadcast_metadata_saved(uint8_t status);

/// Notify interested clients about the status of metadata checksum recalculation process.
/// @param status Status of the metadata checksum recalculation process
void matoclserv_broadcast_metadata_checksum_recalculated(uint8_t status);

/// Check whether there are any async filesystem operations that still need to finished (e.g delayed chunk operations)
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
