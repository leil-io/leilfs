/*
   Copyright 2026 Leil Storage OÜ

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
#include <set>
#include <utility>
#include <vector>

#include "common/saunafs_version.h"
#include "common/serialization_macros.h"

/// Metadata servers one chunkserver talks to, the configured one included; sizes the connection
/// slots and both job pools' listener tables. Each admitted peer costs three descriptors, a
/// socket plus an eventfd per pool, and a listener's eventfd outlives its peer, so this is also
/// the ceiling on what a malformed snapshot can spend. A chunkserver rejects a snapshot naming
/// this many peers or more, so old and new nodes have to agree on the number.
inline constexpr uint32_t kMaxMetadataConnections = 64;

/// One metadata server as discovery names it: its id, endpoint and software version. The
/// endpoint is an address and never a name, so a metadata server certificate needs an IP
/// SAN for a discovered connection to finish its TLS handshake.
SAUNAFS_DEFINE_SERIALIZABLE_CLASS(MetadataClusterMember, uint32_t, serverId, uint32_t, ip, uint16_t,
                                  port, uint32_t, version);

/// What the configured metadata server tells a chunkserver about its peers.
struct MetadataClusterSnapshot {
	/// Id of the server that produced the snapshot, so a discovered connection can check it
	/// reached the server it was told about.
	uint32_t seedId = 0;
	/// Every other server the chunkserver should connect to.
	std::vector<MetadataClusterMember> members;
};

/// Rejects a snapshot a chunkserver could not act on: no seed, too many members, a member
/// without id or endpoint, one too old for the identity exchange, or a repeated id or endpoint.
inline bool isValidMetadataClusterSnapshot(uint32_t seedId,
                                           const std::vector<MetadataClusterMember> &members) {
	// The seed owns slot zero, so a snapshot may name at most one fewer peer than there are slots.
	if (seedId == 0 || members.size() >= kMaxMetadataConnections) { return false; }

	std::set<uint32_t> identities{seedId};
	std::set<std::pair<uint32_t, uint16_t>> endpoints;
	for (const auto &member : members) {
		if (member.serverId == 0 || member.ip == 0 || member.port == 0 ||
		    member.version < kFirstVersionWithChunkserverIdentity) {
			return false;
		}
		if (!identities.insert(member.serverId).second ||
		    !endpoints.emplace(member.ip, member.port).second) {
			return false;
		}
	}
	return true;
}
