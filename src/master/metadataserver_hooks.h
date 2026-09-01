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

#include "common/metadataserver_cluster_entry.h"
#include "common/metadataserver_list_entry.h"

/// Returns live MDS endpoints for chunkserver discovery. It is empty in Master and set by
/// the FoundationDB-backed MDS at startup, so Master never publishes a discovery packet.
using MetadataserverClusterMembersHook = std::function<std::vector<MetadataserverClusterEntry>()>;
extern MetadataserverClusterMembersHook gMetadataserverClusterMembersHook;

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
