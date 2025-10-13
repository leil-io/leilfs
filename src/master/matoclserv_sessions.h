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
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <set>
#include <string>

#include "common/generic_lru_cache.h"
#include "common/goal.h"
#include "common/type_defs.h"
#include "master/fs_context.h"

#define SESSION_STATS 16

// From configuration
inline uint32_t gSessionSustainTime;

struct Session {
	using GroupCache = GenericLruCache<uint32_t, FsContext::GroupsContainer, 1024>;
	using OpenFilesSet = std::set<inode_t>;

	uint32_t sessionId;      ///< Session ID
	std::string info;        ///< Mount point path
	std::string mountInfo;   ///< Mount information shown in the CGI, e.g., arguments, mount options
	std::string config;      ///< Session configuration
	uint32_t peerIpAddress;  ///< Peer IP address
	uint16_t peerPort{};     ///< Peer port
	uint8_t newSession;      ///< Indicates if this is a new session (1) or a reconnect (0)
	uint8_t flags;           ///< Session flags. See more details in SFSCommunication.h
	uint8_t minGoal;         ///< Minimum goal allowed for this session
	uint8_t maxGoal;         ///< Maximum goal allowed for this session
	uint32_t minTrashTime;   ///< Minimum time in seconds to keep files in trash
	uint32_t maxTrashTime;   ///< Maximum time in seconds to keep files in trash
	uint32_t rootUid;        ///< Remapped UID of the user who created the session
	uint32_t rootGid;        ///< Remapped GID of the user who created the session
	uint32_t mapAllUid;  ///< UID to map all non-root users to when the session has SESFLAG_MAPALL
	                     ///< flag set)
	uint32_t mapAllGid;  ///< GID to map all non-root users to when the session has SESFLAG_MAPALL
	                     ///< flag set)
	inode_t rootInode;   ///< Special Root Inode with value = 1
	uint32_t disconnectedTimestamp;  ///< Last connected timestamp for this session
	                                 ///< A value = 0 means a client is connected
	                                 ///< A value > 0 means the last disconnection timestamp
	uint32_t connections;  ///< Number of active connections. A value of 0 means no connections
	std::array<uint32_t, SESSION_STATS> currHourOperationsStats;  ///< Current hour operations stats
	std::array<uint32_t, SESSION_STATS>
	    prevHourOperationsStats;  ///< Previous hour operations stats
	GroupCache groupsCache;       ///< Cache for groups ID for this session
	OpenFilesSet openFilesSet;    ///< Set of open files for this session

	Session()
	    : sessionId(),
	      info(),
	      peerIpAddress(),
	      newSession(),
	      flags(),
	      minGoal(GoalId::kMin),
	      maxGoal(GoalId::kMax),
	      minTrashTime(),
	      maxTrashTime(std::numeric_limits<uint32_t>::max()),
	      rootUid(),
	      rootGid(),
	      mapAllUid(),
	      mapAllGid(),
	      rootInode(SPECIAL_INODE_ROOT),
	      disconnectedTimestamp(),
	      connections(),
	      currHourOperationsStats(),
	      prevHourOperationsStats(),
	      groupsCache(),
	      openFilesSet() {}
};

/// Vector storing all sessions.
inline std::vector<std::unique_ptr<Session>> gSessionsVector;

/// Stores all sessions to a file.
void matoclserv_store_sessions();

/// Loads all sessions from a file.
/// @return 0 on success, -1 on error
int matoclserv_load_sessions();

/// Creates a new session.
/// @param newSession Indicates if this is a new session
/// @param noNewId Indicates if a new session ID should be generated
/// @return Pointer to the newly created session
Session *matoclserv_new_session(uint8_t newSession, uint8_t noNewId);

/// Returns the session for a given ID.
/// @param sessionId The session ID to search for
/// @return Pointer to the session if found, nullptr otherwise
Session *matoclserv_find_session(uint32_t sessionId);

/// Closes a session by its ID.
/// @param sessionId The session ID to close
void matoclserv_close_session(uint32_t sessionId);

/// Returns the number of open files for a given session.
/// @param currentSession Pointer to the session
uint32_t session_number_of_files(Session *currentSession);

/// Rotates the statistics for all sessions (current hour / previous hour) and stores all sessions
/// to a file.
void matocl_session_stats_rotate();

/// Resets the session timeouts for all sessions.
void matoclserv_reset_session_timeouts();

/// Inserts an open file to the list of open files for a given session.
/// @param currentSession Pointer to the session
/// @param inode The inode of the open file
/// @return SAUNAFS_STATUS_OK if the file was successfully acquired, or an error code otherwise
int matoclserv_insert_open_file(Session *currentSession, inode_t inode);

/// Adds an open file to the list of open files for a given session.
/// @param sessionId The ID of the session to which the open file will be added
/// @param inode The inode of the open file to add
void matoclserv_add_open_file(uint32_t sessionId, inode_t inode);

/// Removes an open file from the list of open files for a given session.
/// @param sessionId The ID of the session from which the open file will be removed
/// @param inode The inode of the open file to remove
void matoclserv_remove_open_file(uint32_t sessionId, inode_t inode);

/// Loads and initializes the sessions.
int matoclserv_sessions_init();

/// Clears the sessions.
void matoclserv_session_unload();
