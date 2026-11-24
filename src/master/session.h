/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>

#include "common/generic_lru_cache.h"
#include "common/goal.h"
#include "common/type_defs.h"
#include "master/fs_context.h"

/// Number of per-hour operation counters tracked in each Session.
#define SESSION_STATS 16

/// File-entry budget per cleanup step, shared across sessions and detached-cache eviction.
inline constexpr size_t kSessionReleaseBatchSize = 1000;

/// Shared in-memory representation of one client session.
///
/// This is a shared runtime type used by the session-manager layer, the
/// compatibility wrappers around the legacy session API, and any
/// backend-specific session-manager implementation.
///
/// The active session manager owns `Session` objects and returns raw pointers
/// that remain valid for the session lifetime. `matoclserv` keeps those raw
/// pointers in connection state and updates selected fields during register,
/// reconnect, disconnect, and teardown.
///
/// Persistence is split between backend-defined durable fields and runtime-only
/// fields. Values such as `connections`, `disconnectedTimestamp`,
/// `groupsCache`, and `openFilesSet` are rebuilt or repopulated at runtime.
///
/// `openFilesSet` is the per-session view of open files. It complements the
/// per-file `FSNodeFile::sessionIds` index used by filesystem metadata: the two
/// indexes answer opposite lookup directions and are maintained by open/release
/// paths. KV release views contain one fetched page; their stored index remains
/// authoritative for the files still requiring cleanup.
///
/// Teardown releases the open files in bounded release passes spread over
/// several event-loop steps. Retirement is persisted before file release; the
/// cursor records progress within the current open-file view only in memory.
struct Session {
	/// Cache mapping uid to supplementary groups used when building FsContext.
	using GroupCache = GenericLruCache<uint32_t, FsContext::GroupsContainer, 1024>;

	/// Cached inode set, also used for bounded KV release views.
	using OpenFilesSet = std::set<inode_t>;

	uint32_t sessionId{};  ///< Session ID.
	std::string info;      ///< Mount point path.
	/// Mount information shown in the CGI, such as arguments and mount options.
	std::string mountInfo;
	std::string config;        ///< Session configuration.
	uint32_t peerIpAddress{};  ///< Peer IP address.
	uint16_t peerPort{};       ///< Peer port.
	/// Registration and teardown lifecycle flag used by reconnect and timeout logic.
	uint8_t newSession{};
	/// Session flags. See the `SESFLAG_*` protocol definitions for details.
	uint8_t flags{};
	uint8_t minGoal{GoalId::kMin};  ///< Minimum goal allowed for this session.
	uint8_t maxGoal{GoalId::kMax};  ///< Maximum goal allowed for this session.
	/// Minimum time, in seconds, to keep files in trash.
	uint32_t minTrashTime{};
	/// Maximum time in seconds to keep files in trash.
	uint32_t maxTrashTime{std::numeric_limits<uint32_t>::max()};
	uint32_t rootUid{};  ///< Remapped UID of the user who created the session.
	uint32_t rootGid{};  ///< Remapped GID of the user who created the session.
	/// UID to map all non-root users to when `SESFLAG_MAPALL` is set.
	uint32_t mapAllUid{};
	/// GID to map all non-root users to when `SESFLAG_MAPALL` is set.
	uint32_t mapAllGid{};
	/// Session root inode. Defaults to the global root.
	inode_t rootInode{SPECIAL_INODE_ROOT};
	/// Last disconnection timestamp. Zero means a client is currently connected.
	uint32_t disconnectedTimestamp{};
	/// Active connection count. Zero means the session is currently disconnected.
	uint32_t connections{};
	/// Generation of this server's attachment to the session, raised on every move, reconnect
	/// and release. Zero on backends where a session is not attached to one server (Master).
	uint64_t attachmentGeneration{};
	/// Current hour operation counters.
	std::array<uint32_t, SESSION_STATS> currHourOperationsStats{};
	/// Previous hour operation counters.
	std::array<uint32_t, SESSION_STATS> prevHourOperationsStats{};
	GroupCache groupsCache;     ///< Cache of supplementary groups for this session.
	OpenFilesSet openFilesSet;  ///< Cached open inodes or the current KV release page.
	/// Set when retirement starts, before file release. Reconnects are refused even if
	/// retirement persistence must be retried; cleanup state stays managed until completion.
	bool closing{};
	/// Ensures a release spanning several batches publishes one retirement record.
	bool retirementPublished{};
	/// Next inode in the active view; empty when inactive. KV managers also use zero to queue
	/// a not-yet-loaded page for continuation.
	std::optional<inode_t> releaseCursor;
};
