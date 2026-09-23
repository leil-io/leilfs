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

#include <poll.h>
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "chunkserver/bgjobs.h"
#include "chunkserver/master_connection.h"
#include "protocol/chunks_with_type.h"

class MasterConn;

struct MasterConnCompletionPollPositions {
	int32_t job{-1};
	int32_t replication{-1};
};

/// One metadata server connection and the poll bookkeeping of its job listener.
struct MasterConnSlot {
	std::unique_ptr<MasterConn> connection;
	/// Closed but kept until both pools have no job left for its listener.
	bool retiring = false;
	uint8_t lastLoadFactor = 0;
	MasterConnCompletionPollPositions completionPoll;
};

/// Membership and listener bookkeeping, separate from the event loop so transitions can be tested.
struct MasterConnReconciliationState {
	/// Slot index is the listener id; slot zero is configured and never retires.
	std::array<MasterConnSlot, kMaxMetadataConnections> connections;
	std::vector<MetadataClusterMember> desiredMembers;
	std::vector<int> jobDescriptors;
	std::vector<int> replicationDescriptors;
	/// Retry failed admission on the reconnect tick while existing connections keep being served.
	bool admissionDeferred = false;
};

/// Applies the configured connection's latest snapshot after the current poll batch is served.
void masterconn_reconcile_connections(MasterConnReconciliationState &state,
                                      const std::shared_ptr<MasterJobPool> &jobPool,
                                      const std::shared_ptr<MasterJobPool> &replicationJobPool);

/// Opens another admission attempt and reconnects offline peers that are not retiring.
void masterconn_reconnect_connections(MasterConnReconciliationState &state);

/// Queues the lost report for a copy a failed write had to delete.
JobPool::JobCallback masterconn_jobDeleteAfterErrorFinished(ChunkWithType chunkWithType);

/// Prepares both completion listeners before a peer is admitted. Failure leaves the output
/// descriptors unchanged so the caller can defer admission until its next reconnect tick.
bool masterconn_prepare_listeners(MasterJobPool &jobPool, MasterJobPool &replicationJobPool,
                                  uint32_t listenerId, int &jobDescriptor,
                                  int &replicationDescriptor);

/// Releases one connection and abandons only its listener's replies in both pools, so the other
/// connections keep their jobs. The caller must have marked the connection KILL first, which
/// invalidates the callbacks of the old socket.
void masterconn_close_connection(MasterJobPool &jobPool, MasterJobPool &replicationJobPool,
                                 MasterConn &connection, uint32_t listenerId);

/// Adds the completion descriptors allowed by the connection's durable protocol policy.
void masterconn_add_completion_descriptors(const MasterConn &connection, int jobDescriptor,
                                           int replicationDescriptor,
                                           MasterConnCompletionPollPositions &positions,
                                           std::vector<pollfd> &descriptors);

/// Serves one connection through the same error and completion order as the event loop.
void masterconn_serve_connection(MasterJobPool &jobPool, MasterJobPool &replicationJobPool,
                                 MasterConn &connection, uint32_t listenerId,
                                 const MasterConnCompletionPollPositions &positions,
                                 const std::vector<pollfd> &descriptors);

/// Evaluates shutdown readiness for the supplied connection set and shared job pools.
bool masterconn_can_exit(MasterJobPool &jobPool, MasterJobPool &replicationJobPool,
                         std::span<MasterConn *const> connections);

void masterconn_stats(uint64_t *bin, uint64_t *bout, uint32_t *maxjobscnt);

/// Peak-sampling rule for the job-count chart. The peak stands unchanged while no connection is
/// established, since nothing queued can reach a metadata server to be counted against it.
uint32_t masterconn_sample_max_jobs_count(bool anyConnected, uint32_t previousMax,
                                          uint32_t jobCount, uint32_t replicationJobCount);
int masterconn_init(void);
int masterconn_init_threads(void);
MasterJobPool* masterconn_get_job_pool();
bool masterconn_canexit();
