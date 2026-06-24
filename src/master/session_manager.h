/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "common/sessions_file.h"
#include "master/session.h"

class FilesystemOperationContext;

/// Interface for session management.
///
/// Classes implementing this interface own the lifecycle of every client session
/// served by the master: creation, lookup, timeout handling, persistence, and
/// the per-session open-files index. It is the integration point between the
/// network layer in `matoclserv` and the selected metadata backend. The
/// master-side session dispatcher forwards every session call through the
/// single active implementation, which is selected at startup by the backend
/// wiring.
///
/// Implementations must be safe to call from the master event loop; they are not
/// expected to be thread-safe across arbitrary threads at this point.
class ISessionManager {
public:
	/// Default constructor.
	ISessionManager() = default;

	/// Unneeded copy/assign/move constructors and operators.
	ISessionManager(const ISessionManager &) = delete;
	ISessionManager &operator=(const ISessionManager &) = delete;
	ISessionManager(ISessionManager &&) = delete;
	ISessionManager &operator=(ISessionManager &&) = delete;

	/// Virtual destructor.
	virtual ~ISessionManager() = default;

	/// Prepares the backing store and primes the in-memory session set.
	///
	/// Called exactly once from matoclserv_sessions_init(), after a concrete
	/// session-manager implementation has been attached. The exact startup point
	/// is backend-specific. Typical implementations invoke loadSessions() and
	/// react to its outcome (missing store, loaded, error).
	///
	/// @return An implementation-defined startup status. The dispatcher
	///         propagates this value to the RunTab. Implementations may log
	///         loadSessions() failures and still return 0 to preserve legacy
	///         startup behaviour.
	virtual int initialize() = 0;

	/// Loads all persisted sessions from the backing store into memory.
	///
	/// @return A backend-defined load status. Callers should treat this as an
	///         implementation detail, apart from the conventional success/error
	///         distinction used by initialize().
	virtual int loadSessions() = 0;

	/// Persists all sessions to the backing store.
	///
	/// Only sessions with @c newSession==1 are written. The exact persistence
	/// semantics are backend-specific.
	virtual void storeSessions() = 0;

	/// Persists one session through the active backend.
	///
	/// Backends that only support full-catalog rewrites may forward this to
	/// storeSessions(). Backends with row-level persistence can upsert just the
	/// requested session.
	virtual void persistSession(const Session &session) = 0;

	/// Persists or observes runtime state after an active connection disconnects.
	///
	/// The file backend keeps this as a no-op because its restart semantics are
	/// driven by the persisted session catalog. KV backends may use this to
	/// persist runtime ownership/disconnect state separately from the stable
	/// session record.
	virtual void sessionDisconnected(const Session &session) = 0;

	/// Creates a new session and appends it to the managed set.
	///
	/// @param newSession Value copied into Session::newSession. Current
	///                   registration paths pass 1 for both regular and meta
	///                   sessions. The combination (@p newSession == 0 &&
	///                   @p noNewId != 0) is a compatibility mode for
	///                   session-id-less synthetic entries.
	/// @param noNewId When non-zero together with @p newSession==0, the returned
	///                session keeps sessionId=0 and no id is allocated from the
	///                backend. Otherwise a fresh id is drawn from
	///                IFilesystemOperations::newSessionId().
	///
	/// @return A pointer to the newly-added Session owned by this manager, or
	///         nullptr if id allocation failed.
	virtual Session *newSession(uint8_t newSession, uint8_t noNewId) = 0;

	/// Returns the session matching @p sessionId, preparing it for reuse.
	///
	/// On hit, this increments @c connections, clears @c disconnectedTimestamp,
	/// and clears the closed-session marker in Session::newSession when present
	/// (subtracts 2 if @c newSession >= 2).
	///
	/// @return A pointer to the matching Session owned by this manager, or
	///         nullptr if no session has the given id.
	virtual Session *findSession(uint32_t sessionId) = 0;

	/// Marks the session identified by @p sessionId as explicitly closed.
	///
	/// This only updates Session::newSession when the session exists, has exactly
	/// one active connection, and is not already marked. It does not decrement
	/// @c connections or set @c disconnectedTimestamp; that is done by
	/// matocl_before_disconnect(). Timeout-based reaping is handled by
	/// removeTimedOutSessions(). If @p sessionId is 0 or unknown this is a no-op.
	virtual void closeSession(uint32_t sessionId) = 0;

	/// Registers @p inode as open on @p currentSession, delegating the
	/// persistent portion of the acquisition to the metadata backend.
	///
	/// If the session already tracks @p inode this returns immediately without
	/// calling into the backend. Otherwise it invokes
	/// IFilesystemOperations::acquire() inside the provided context; on
	/// SAUNAFS_STATUS_OK it inserts @p inode into Session::openFilesSet.
	///
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param currentSession Session that owns the opened file. Must be non-null.
	/// @param inode The inode number of the file being acquired.
	///
	/// @return SAUNAFS_STATUS_OK on success, otherwise the status returned by
	///         IFilesystemOperations::acquire().
	virtual int insertOpenFile(const FilesystemOperationContext &fsOpContext,
	                           Session *currentSession, inode_t inode) = 0;

	/// Backend-persist-only half of insertOpenFile: runs IFilesystemOperations::acquire()
	/// inside @p fsOpContext but does NOT touch Session::openFilesSet. The caller
	/// records the in-memory open file (recordOpenFile) only after the transaction
	/// commits, so a commit retry can replay the persistent acquire on a fresh
	/// transaction without the already-open early-return skipping it.
	virtual int acquireOpenFilePersist(const FilesystemOperationContext &fsOpContext,
	                                   Session *currentSession, inode_t inode) = 0;

	/// In-memory half of insertOpenFile: records @p inode in @p currentSession's
	/// openFilesSet. The caller pairs this with acquireOpenFilePersist(), invoking
	/// it only after the persisting transaction commits. Unlike addOpenFile() it
	/// takes the live Session pointer the caller already holds, so it never goes
	/// through the by-id lookup / synthetic-session recovery path (which the KV
	/// backend, whose sessions are loaded on demand, refuses).
	virtual void recordOpenFile(Session *currentSession, inode_t inode) = 0;

	/// Adds @p inode to Session::openFilesSet for the session @p sessionId.
	///
	/// Used during startup reconstruction, restore/shadow replay, and similar
	/// paths where the file-side acquisition is already persisted and only the
	/// in-memory index needs to be rebuilt. If no session with @p sessionId
	/// exists, a synthetic placeholder session is created with
	/// @c disconnectedTimestamp set to "now". This path preserves legacy
	/// behaviour for pre-1.5.13 clients.
	virtual void addOpenFile(uint32_t sessionId, inode_t inode) = 0;

	/// Removes @p inode from Session::openFilesSet for the session @p sessionId.
	///
	/// Mirror of addOpenFile(). If no session is found the manager logs a
	/// warning with the missing session id and inode but does not abort. This is
	/// safe to call when the inode is absent from the set.
	virtual void removeOpenFile(uint32_t sessionId, inode_t inode) = 0;

	/// Refreshes disconnectedTimestamp for every managed session to "now".
	///
	/// Currently used when the process becomes active master again, to avoid
	/// immediate timeout of sessions restored from a previously passive role.
	virtual void resetSessionTimeouts() = 0;

	/// Rolls per-session operation stats from current-hour into previous-hour
	/// and zeroes the current-hour counters, then persists the result.
	virtual void rotateStats() = 0;

	/// Drops all in-memory session state without touching the backing store.
	///
	/// Used both at shutdown and as the first step of initialize()/re-load.
	virtual void unload() = 0;

	/// Invokes @p visitor on every managed session, in insertion order.
	///
	/// The visitor receives a @c const reference; modifying session state from
	/// here is not supported.
	virtual void forEachSession(const std::function<void(const Session &)> &visitor) const = 0;

	/// Returns session summaries for the admin list-sessions command.
	///
	/// Backends may choose whether this is local-process or cluster-wide. The
	/// file backend exposes the current local in-memory view; KV backends could
	/// expose the full cluster view (implementation dependent).
	virtual std::vector<SessionFiles> listSessions() const = 0;

	/// Returns true when listSessions() should replace matoclserv's local
	/// connection-list walk for admin list-sessions.
	virtual bool usesBackendSessionList() const = 0;

	/// Reaps sessions whose disconnect-timeout has elapsed.
	///
	/// For each session whose disconnect deadline is in the past, the manager
	/// first invokes @p onTimedOut (so the network layer can close any lingering
	/// open files). If the callback returns true, the entry is removed. If it
	/// returns false, the session stays managed so a later sweep can retry.
	///
	/// @param now Current wall-clock time (seconds since epoch), typically from
	///            eventloop_time().
	/// @param sessionSustainTime How long a disconnected session is kept alive
	///                           before it is eligible for reaping. Clamped by
	///                           configuration on the dispatcher side.
	/// @param onTimedOut Invoked for every timed-out session. Return true when
	///                   teardown completed and the session may be removed.
	virtual void removeTimedOutSessions(uint32_t now, uint32_t sessionSustainTime,
	                                    const std::function<bool(Session *)> &onTimedOut) = 0;

protected:
	/// Guard against construction from a null reference; kept to mirror the
	/// IFilesystemOperations pattern and prevent surprising coercions.
	ISessionManager(const std::nullptr_t &) = delete;
};

/// Base implementation of ISessionManager shared by every backend.
///
/// Provides the in-memory session vector, the common id-allocation and
/// open-files bookkeeping, and the timeout-reaping loop. Concrete subclasses
/// only need to supply the persistence side: initialize(), loadSessions(),
/// storeSessions(), plus, optionally, onSessionRemoved() to tear down any
/// backend-specific per-session state and report whether removal can finish.
///
/// Newly derived classes should prefer extending this base rather than
/// implementing ISessionManager from scratch, so different backends keep the
/// same in-memory semantics.
class SessionManagerBase : public ISessionManager {
public:
	/// Default constructor.
	SessionManagerBase() = default;

	/// @copydoc ISessionManager::newSession
	/// @note Delegates id allocation to IFilesystemOperations::newSessionId().
	///       Returns nullptr when the allocator returns 0.
	Session *newSession(uint8_t newSession, uint8_t noNewId) override;

	/// @copydoc ISessionManager::findSession
	Session *findSession(uint32_t sessionId) override;

	/// @copydoc ISessionManager::closeSession
	void closeSession(uint32_t sessionId) override;

	/// @copydoc ISessionManager::insertOpenFile
	int insertOpenFile(const FilesystemOperationContext &fsOpContext, Session *currentSession,
	                   inode_t inode) override;

	/// @copydoc ISessionManager::acquireOpenFilePersist
	int acquireOpenFilePersist(const FilesystemOperationContext &fsOpContext,
	                           Session *currentSession, inode_t inode) override;

	/// @copydoc ISessionManager::recordOpenFile
	void recordOpenFile(Session *currentSession, inode_t inode) override;

	/// @copydoc ISessionManager::addOpenFile
	void addOpenFile(uint32_t sessionId, inode_t inode) override;

	/// @copydoc ISessionManager::removeOpenFile
	void removeOpenFile(uint32_t sessionId, inode_t inode) override;

	/// @copydoc ISessionManager::resetSessionTimeouts
	void resetSessionTimeouts() override;

	/// @copydoc ISessionManager::rotateStats
	/// @note The default implementation preserves legacy behavior by calling
	///       storeSessions() after rotating the in-memory counters. Backends
	///       with row-level persistence may override this.
	void rotateStats() override;

	/// @copydoc ISessionManager::persistSession
	/// @note Defaults to storeSessions() to preserve the file-backed contract.
	void persistSession(const Session &session) override;

	/// @copydoc ISessionManager::sessionDisconnected
	/// @note Defaults to a no-op to preserve the file-backed contract.
	void sessionDisconnected(const Session &session) override;

	/// @copydoc ISessionManager::unload
	void unload() override;

	/// @copydoc ISessionManager::forEachSession
	void forEachSession(const std::function<void(const Session &)> &visitor) const override;

	/// @copydoc ISessionManager::listSessions
	std::vector<SessionFiles> listSessions() const override;

	/// @copydoc ISessionManager::usesBackendSessionList
	bool usesBackendSessionList() const override;

	/// @copydoc ISessionManager::removeTimedOutSessions
	/// @note Calls @p onTimedOut first. If teardown and onSessionRemoved()
	///       both succeed, erases the entry. Otherwise keeps the session for a
	///       later retry.
	void removeTimedOutSessions(uint32_t now, uint32_t sessionSustainTime,
	                            const std::function<bool(Session *)> &onTimedOut) override;

	/// Appends @p sessionPtr to the managed vector and returns a non-owning
	/// pointer to the stored Session. @p sessionPtr must be non-null.
	///
	/// Primary callers in production code are newSession() and the synthetic
	/// addOpenFile() recovery path. Exposed publicly so that tests (and other
	/// out-of-tree extensions) can populate the manager without going through
	/// newSession(), which requires gFSOperations.
	Session *addSession(std::unique_ptr<Session> sessionPtr);

protected:
	/// Looks up the session with @p sessionId without touching any fields.
	/// Returns nullptr for sessionId==0 or when the id is unknown.
	Session *findSessionEntry(uint32_t sessionId);

	/// @copydoc SessionManagerBase::findSessionEntry(uint32_t)
	const Session *findSessionEntry(uint32_t sessionId) const;

	/// Hook invoked after timed-out session teardown succeeds and before the
	/// session is erased from the in-memory vector.
	///
	/// The default implementation returns true, it is used by the file
	/// backend where persistence is driven by storeSessions(). KV backends
	/// override this to drop the session record and its open-file index rows,
	/// and return false if durable cleanup failed.
	virtual bool onSessionRemoved(const Session &session);

	/// Returns true when @p session is eligible for reaping under the tri-level
	/// threshold (freshly-registered pending, normal, and legacy pre-1.5.13).
	static bool isTimedOut(const Session &session, uint32_t now, uint32_t sessionSustainTime);

protected:
	/// The owned, insertion-ordered set of sessions.
	std::vector<std::unique_ptr<Session>> sessions_;
};

/// File-backed session manager used by the classic master personality.
///
/// Persists sessions to @c sessions.sfs (see @c kSessionsFilename) next to the
/// metadata image, using the SFSSIGNATURE "S \001\006\004" format. There is no
/// per-session open-file index on disk. After restart, Session::openFilesSet is
/// rebuilt from FSNodeFile::sessionIds during metadata loading, and the
/// base-class onSessionRemoved() hook is intentionally a no-op.
class SessionManagerFile final : public SessionManagerBase {
public:
	/// @copydoc ISessionManager::initialize
	/// @note On fresh installs (sessions file missing) this logs a warning and
	///       writes an empty file so subsequent runs find it.
	int initialize() override;

	/// @copydoc ISessionManager::loadSessions
	int loadSessions() override;

	/// @copydoc ISessionManager::storeSessions
	/// @note Atomic rename from the ".tmp" companion. Partial write failures
	///       leave the existing sessions file untouched.
	void storeSessions() override;
};
