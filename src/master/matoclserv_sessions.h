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
#include <functional>
#include <memory>
#include <vector>

#include "common/sessions_file.h"
#include "master/session.h"

// From configuration
inline uint32_t gSessionSustainTime;

class FilesystemOperationContext;
class ISessionManager;

/// Overrides the active session manager implementation.
void matoclserv_set_session_manager(std::unique_ptr<ISessionManager> manager);

/// Persists all sessions through the active session manager.
void matoclserv_store_sessions();

/// Persists one session through the active session manager.
void matoclserv_persist_session(Session *session);

/// Persists or observes session runtime state after a connection disconnects.
void matoclserv_session_disconnected(Session *session);

/// Loads all sessions from the active session manager's backing store.
/// @return The active session manager's backend-defined load status.
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

/// Marks a session as explicitly closed in manager state.
/// @param sessionId The session ID to mark as closed
void matoclserv_close_session(uint32_t sessionId);

/// Returns the number of open files for a given session.
/// @param currentSession Pointer to the session
uint32_t session_number_of_files(Session *currentSession);

/// Rotates the statistics for all sessions (current hour / previous hour) and
/// persists them through the active session manager.
void matocl_session_stats_rotate();

/// Resets the session timeouts for all sessions.
void matoclserv_reset_session_timeouts();

/// Iterates over all sessions currently owned by the session manager.
void matoclserv_for_each_session(const std::function<void(const Session &)> &visitor);

/// Returns session summaries for the admin list-sessions command.
std::vector<SessionFiles> matoclserv_list_sessions();

/// Returns true when the active backend provides the admin list-sessions view.
bool matoclserv_uses_backend_session_list();

/// Removes timed out sessions whose callback confirms teardown completed.
void matoclserv_remove_timed_out_sessions(const std::function<bool(Session *)> &onTimedOut);

/// Inserts an open file to the list of open files for a given session.
/// @param currentSession Pointer to the session
/// @param inode The inode of the open file
/// @return SAUNAFS_STATUS_OK if the file was successfully acquired, or an error code otherwise
int matoclserv_insert_open_file(const FilesystemOperationContext &fsOpContext,
                                Session *currentSession, inode_t inode);

/// Backend-persist-only half of matoclserv_insert_open_file: acquires the file in the
/// transaction without recording it in Session::openFilesSet. Pair with
/// matoclserv_record_open_file() on commit success so a commit retry can replay the
/// persistent acquire on a fresh transaction. @return acquire() status.
int matoclserv_acquire_open_file_persist(const FilesystemOperationContext &fsOpContext,
                                         Session *currentSession, inode_t inode);

/// In-memory half paired with matoclserv_acquire_open_file_persist: records the
/// open file in @p currentSession on commit success. Takes the live Session
/// pointer (not a session id), so it avoids the by-id / synthetic-session path
/// that the KV backend refuses.
void matoclserv_record_open_file(Session *currentSession, inode_t inode);

/// Adds an open file to the list of open files for a given session.
/// @param sessionId The ID of the session to which the open file will be added
/// @param inode The inode of the open file to add
void matoclserv_add_open_file(uint32_t sessionId, inode_t inode);

/// Removes an open file from the list of open files for a given session.
/// @param sessionId The ID of the session from which the open file will be removed
/// @param inode The inode of the open file to remove
void matoclserv_remove_open_file(uint32_t sessionId, inode_t inode);

/// Initializes the active session manager and configures SESSION_SUSTAIN_TIME.
int matoclserv_sessions_init();

/// Reads SESSION_SUSTAIN_TIME from config and clamps it into the allowed range.
/// Shared between session init and reload so both paths stay in sync.
void matoclserv_configure_session_sustain_time();

/// Clears in-memory session state held by the active session manager.
void matoclserv_session_unload();
