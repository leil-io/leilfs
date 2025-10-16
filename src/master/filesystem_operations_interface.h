/*
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

#include <map>
#include <memory>

#include "common/goal.h"
#include "master/fs_context.h"
#include "master/locks.h"

/// Interface for filesystem operations extensibility.
/// Classes implementing this interface can be used to override default filesystem behavior.
class IFilesystemOperations {
public:
	/// Default constructor
	IFilesystemOperations() = default;

	/// Unneeded copy/assign constructors/operators
	IFilesystemOperations(const IFilesystemOperations &) = delete;
	IFilesystemOperations &operator=(const IFilesystemOperations &) = delete;
	IFilesystemOperations(IFilesystemOperations &&) = delete;
	IFilesystemOperations &operator=(IFilesystemOperations &&) = delete;

	/// Virtual destructor
	virtual ~IFilesystemOperations() = default;

#ifndef METARESTORE
	// Goals

	/// Returns a map with all defined goals.
	virtual const std::map<int, Goal> &fs_get_goal_definitions() const = 0;

	/// Returns goal definition for given goal id.
	virtual const Goal &fs_get_goal_definition(uint8_t goalId) const = 0;
#endif

	// Lock operations

	/// Perform a flock-style (whole-file) advisory lock operation.
	///
	/// Supports placing and removing whole-file shared/exclusive advisory locks, removing pending
	/// requests and handling interruptible requests.
	/// @param context Filesystem context.
	/// @param inode Target inode for the flock operation.
	/// @param owner Owner identifier provided by the client.
	/// @param sessionid Session id of the requesting client.
	/// @param reqid Request id (used to identify interruptible requests).
	/// @param msgid Message id provided by the client (used for interrupt handling).
	/// @param oper Operation code: expected values include: safs_locks::{kShared, kExclusive,
	/// kUnlock, kRelease}.
	/// @param nonblocking If true, do not block, return immediately if the lock would block.
	/// @param applied On success (and after unlocking) contains owners of any pending
	///                locks that were applied as a side-effect.
	/// @return `SAUNAFS_STATUS_OK` on success, `SAUNAFS_ERROR_WAITING` if the request
	///         cannot be granted immediately, `SAUNAFS_ERROR_EINVAL` for invalid args,
	///         or other filesystem-specific error codes (permission, quota, etc.).
	virtual int fs_flock_op(const FsContext &context, inode_t inode, uint64_t owner,
	                        uint32_t sessionid, uint32_t reqid, uint32_t msgid, uint16_t oper,
	                        bool nonblocking, std::vector<FileLocks::Owner> &applied) = 0;

	/// Perform a POSIX byte-range (fcntl) lock operation on the filesystem.
	///
	/// Handles POSIX (fcntl) style byte-range locks: place shared/exclusive locks,
	/// release ranges, enqueue pending requests and handle interruptible requests.
	/// @param context Filesystem context (permissions, timestamp, personality, ...).
	/// @param inode Target inode on which the byte-range lock is requested.
	/// @param start Start offset of the range (inclusive).
	/// @param end End offset of the range (exclusive).
	/// @param owner Owner identifier provided by the client (FUSE owner).
	/// @param sessionid Session id of the requesting client.
	/// @param reqid Request id (used to identify interruptible requests).
	/// @param msgid Message id provided by the client (used for interrupt handling).
	/// @param oper Operation code: expected values include: safs_locks::{kShared, kExclusive,
	/// kUnlock, kRelease}.
	/// @param nonblocking If true, do not block, return immediately if the lock would block.
	/// @param applied When pending locks become applied as a result of this operation,
	///                their owners are appended to this vector.
	/// @return `SAUNAFS_STATUS_OK` on success, `SAUNAFS_ERROR_WAITING` if the request
	///         cannot be granted immediately, `SAUNAFS_ERROR_EINVAL` for invalid args,
	///         or other filesystem-specific error codes (permission, quota, etc.).
	virtual int fs_posixlock_op(const FsContext &context, inode_t inode, uint64_t start,
	                            uint64_t end, uint64_t owner, uint32_t sessionid, uint32_t reqid,
	                            uint32_t msgid, uint16_t oper, bool nonblocking,
	                            std::vector<FileLocks::Owner> &applied) = 0;

	/// Perform a POSIX lock probe on filesystem.
	/// A POSIX probe checks whether a lock request (shared/exclusive) would be blocked
	/// by existing locks without placing a lock.
	/// @param context Filesystem context.
	/// @param inode Inode number on which to probe locks.
	/// @param start Start of the range to probe.
	/// @param end End of the range to probe.
	/// @param owner Owner identifier provided by the client (FUSE owner typically).
	/// @param sessionid Session id of the client that is probing the lock.
	/// @param reqid Request id (used to identify interruptible requests).
	/// @param msgid Message id provided by the client.
	/// @param oper Operation code: one of safs_locks::kShared, safs_locks::kExclusive or
	/// safs_locks::kUnlock. The probe checks for conflicts for the requested lock type.
	/// @param info Wrapper around 'struct flock' (safs_locks::FlockWrapper). If a conflicting lock
	///             exists, 'info' is filled with the conflicting lock type, start and length.
	/// @return SAUNAFS_STATUS_OK if no conflicting lock was found (info.l_type set to kUnlock),
	///         SAUNAFS_ERROR_WAITING if a conflicting lock was found (info filled),
	///         SAUNAFS_ERROR_EINVAL for invalid parameters.
	virtual int fs_posixlock_probe(const FsContext &context, inode_t inode, uint64_t start,
	                               uint64_t end, uint64_t owner, uint32_t sessionid, uint32_t reqid,
	                               uint32_t msgid, uint16_t oper,
	                               safs_locks::FlockWrapper &info) = 0;

	/// Release (unlock + unqueue) all locks from a given session.
	/// @param context Filesystem context.
	/// @param type Type of locks to clear (kFlock, kPosix).
	/// @param inode inode number on which to clear locks.
	/// @param sessionid Session id whose locks are to be cleared.
	/// @param applied Vector to be filled with the owners of the cleared locks.
	virtual int fs_locks_clear_session(const FsContext &context, uint8_t type, inode_t inode,
	                                   uint32_t sessionid,
	                                   std::vector<FileLocks::Owner> &applied) = 0;

	/// List locks in the filesystem.
	/// Fills outLocks with locks matching the type and pending parameters.
	/// @param context Filesystem context (could be ignored in some implementations).
	/// @param type Type of locks to list (kFlock, kPosix).
	/// @param pending If true, lists pending locks, otherwise lists active locks.
	/// @param start Start index for listing.
	/// @param max Maximum number of locks to list.
	/// @param outLocks Vector to be filled with the listed locks.
	virtual int fs_locks_list_all(const FsContext &context, uint8_t type, bool pending,
	                              uint64_t start, uint64_t max,
	                              std::vector<safs_locks::Info> &outLocks) = 0;

	/// List locks for a specific inode.
	/// @param context Filesystem context (could be ignored in some implementations).
	/// @param type Type of locks to list (kFlock, kPosix).
	/// @param pending If true, lists pending locks, otherwise lists active locks.
	/// @param inode inode number on which to list locks.
	/// @param start Start index for listing.
	/// @param max Maximum number of locks to list.
	/// @param outLocks Vector to be filled with the listed locks.
	virtual int fs_locks_list_inode(const FsContext &context, uint8_t type, bool pending,
	                                inode_t inode, uint64_t start, uint64_t max,
	                                std::vector<safs_locks::Info> &outLocks) = 0;

	/// Unlocks the matching locks on the specified inode and tries to apply pending locks.
	/// @param context Filesystem context.
	/// @param type Type of locks to unlock (kFlock, kPosix).
	/// @param inode inode number on which to unlock locks.
	/// @param applied Vector to be filled with the owners of the unlocked locks.
	virtual int fs_locks_unlock_inode(const FsContext &context, uint8_t type, inode_t inode,
	                                  std::vector<FileLocks::Owner> &applied) = 0;

	/// Removes a pending lock matching the provided parameters.
	/// @param context Filesystem context.
	/// @param type Type of lock to operate on (kFlock, kPosix).
	/// @param ownerid Owner identifier provided by the client (FUSE owner typically).
	/// @param sessionid Session id of the client that enqueued the lock.
	/// @param inode Inode number on which the pending lock was queued.
	/// @param reqid Request id (used to identify interruptible requests).
	virtual int fs_locks_remove_pending(const FsContext &context, uint8_t type, uint64_t ownerid,
	                                    uint32_t sessionid, inode_t inode, uint64_t reqid) = 0;
};

// Global filesystem operations instance.
// This global unique_ptr is initialized once at startup (before any FS calls) and set to a single
// concrete implementation for the process lifetime. It must not be reassigned and its dynamic type
// remains stable, so callers may assume one immutable implementation.
inline std::unique_ptr<IFilesystemOperations> gFilesystemOperations = nullptr;
