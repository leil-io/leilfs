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
#include <functional>
#include <optional>
#include <vector>

#include "common/distributed_chunkserver_session.h"
#include "common/metadataserver_cluster_entry.h"
#include "common/metadataserver_list_entry.h"

/// Returns the list of other known metadata servers. Defaults to today's Master/Shadow
/// logic (matomlserv_shadows), a leil-mds reassigns this at startup to read its
/// FoundationDB-backed cluster registry instead. A plain hook, not a virtual interface,
/// since this is one stateless, read-only query with no state spanning calls.
/// May throw when the underlying read fails or refuses (e.g. an unreachable backend, or a
/// scan that would exceed its row cap): the dispatch site translates any exception into
/// dropping that one admin connection, so the caller sees a failed query, never a result
/// indistinguishable from an empty cluster.
using MetadataserversListHook = std::function<std::vector<MetadataserverListEntry>()>;
extern MetadataserversListHook gMetadataserversListHook;

/// Per-instance identity a metadataserver-status reply carries only when the requester
/// understands the newer packet version. Populated only by the KV-mode hook.
struct MetadataserverIdentity {
	uint32_t mdsId = 0;
	/// Random per process start, so successive processes of one stable id are distinct. The batch
	/// witness keys by it (with a checked sequence) so a later process cannot read an older one's
	/// witness rows as its own.
	uint64_t incarnation = 0;
};

/// This instance's status (SAU_METADATASERVER_STATUS_MASTER/SHADOW_*) and, when known,
/// its identity.
struct MetadataserverStatusResult {
	uint8_t status = 0;
	std::optional<MetadataserverIdentity> identity;
};

/// Returns this instance's status. Defaults to today's Master/Shadow logic
/// (isMaster/masterconn_is_connected), identity left empty, a leil-mds reassigns this at
/// startup to also report its mds_id. Same failure contract as gMetadataserversListHook:
/// an implementation may throw, and the dispatch site drops that one connection.
using MetadataserverStatusHook = std::function<MetadataserverStatusResult()>;
extern MetadataserverStatusHook gMetadataserverStatusHook;

/// The distributed cluster view pushed to chunkservers: this instance's own id and every
/// live member, self included, each with its chunkserver-facing port. Unset by default: a
/// Master/Shadow deployment has no such push and never sends the packet. A leil-mds
/// reassigns this at startup to read its FoundationDB-backed cluster registry. Same
/// failure contract as the other hooks: an implementation may throw, and the dispatch
/// site skips that one broadcast tick.
struct MetadataserverClusterView {
	uint32_t selfMdsId = 0;
	std::vector<MetadataserverClusterEntry> members;
};
using MetadataserverClusterViewHook = std::function<MetadataserverClusterView()>;
extern MetadataserverClusterViewHook gMetadataserverClusterViewHook;

/// Correctness-bearing request passed only by the distinct distributed registration
/// handler. Network address, cluster and packet-shape validation remain in matocsserv;
/// this hook owns the FoundationDB mint/claim decision.
struct ChunkserverSessionRegistrationRequest {
	uint32_t stableId = 0;
	uint64_t chunkserverIncarnation = 0;
	DistributedRegistrationRole role = DistributedRegistrationRole::kMintOnly;
	bool ready = false;
	uint64_t scanEpoch = 0;
};

/// A mint-only success has stableId != 0 and both claim fields zero. An admission
/// success has every identity and claim field nonzero. Any other shape is rejected by
/// the public dispatch site even if a faulty hook reports status OK.
struct ChunkserverSessionRegistrationResult {
	uint8_t status = 0;
	uint32_t stableId = 0;
	uint32_t mdsId = 0;
	uint64_t mdsIncarnation = 0;
	uint64_t claimSequence = 0;
	uint64_t leaseDeadline = 0;
	/// Clock tolerance plus drain bound, forwarded to the chunkserver so both sides
	/// derive the same conservative cutoff from the durable deadline.
	uint64_t cutoffReserveSeconds = 0;
	/// Absolute session-authority second after which this MDS must not offer the
	/// server for locate or dispatch (leaseDeadline minus the reserve, checked).
	uint64_t dispatchCutoff = 0;
};

/// Invoked exactly once with the registration outcome, on the MDS network event loop,
/// after the durable decision resolved off that loop. The dispatch site must tolerate
/// the originating connection having died in the meantime.
using ChunkserverSessionRegistrationCompletion =
    std::function<void(const ChunkserverSessionRegistrationResult &)>;

/// Unset in Master/Shadow mode. Its presence is also the public-side proof that this
/// server is an MDS and must explicitly refuse the legacy registration packet before
/// ingesting reports or inventory. The hook only submits: no FoundationDB read, write
/// or commit may run inside it, and the completion fires on a later loop iteration.
using ChunkserverSessionRegistrationHook = std::function<void(
    const ChunkserverSessionRegistrationRequest &, ChunkserverSessionRegistrationCompletion)>;
extern ChunkserverSessionRegistrationHook gChunkserverSessionRegistrationHook;

/// Fire-and-forget clean-shutdown release of one exact session-claim tuple: the durable
/// claim is expired conditionally on that exact tuple so a graceful restart's successor
/// incarnation is admitted without waiting out the lease. A lost or stale release is
/// harmless; fencing then applies as usual. Unset in Master/Shadow mode; submit-only,
/// like the registration hook.
