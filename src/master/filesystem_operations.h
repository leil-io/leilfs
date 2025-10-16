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

#include <map>

#include "common/goal.h"
#include "common/type_defs.h"
#include "master/filesystem_operations_interface.h"
#include "master/fs_context.h"
#include "master/locks.h"
#include "protocol/lock_info.h"

#define DEFAULT_GOAL 1

/// Default trashtime of 26 hours and 17 minutes.
/// This time is selected for two main reasons:
/// 1. Avoid overlapping daily scheduled file deletions with actual chunk deletions from the
///    previous day.
/// 2. Reduce collisions of chunk deletions and metadata dump operations.
constexpr uint32_t kDefaultTrashTime = 94620;

/// Base class for filesystem operations extensibility.
///
/// Provides default implementations for all methods of IFilesystemOperations interface.
/// Will grow with time as new methods are added to the interface. Most likely, will be split
/// in the future or implemented by specialized classes, like FilesystemOperationsInMemory and/or
/// FilesystemOperationsKV.
class FilesystemOperationsBase : public IFilesystemOperations {
public:
#ifndef METARESTORE
	// Goals

	/// Returns a map with all defined goals.
	const std::map<int, Goal> &fs_get_goal_definitions() const override;

	/// Returns goal definition for given goal id.
	const Goal &fs_get_goal_definition(uint8_t goalId) const override;
#endif

	// Locks

	/// Perform a flock operation on filesystem.
	/// @see IFilesystemOperations::fs_flock_op.
	int fs_flock_op(const FsContext &context, inode_t inode, uint64_t owner, uint32_t sessionid,
	                uint32_t reqid, uint32_t msgid, uint16_t oper, bool nonblocking,
	                std::vector<FileLocks::Owner> &applied) override;

	/// Perform a posix lock operation on filesystem.
	/// @see IFilesystemOperations::fs_posixlock_op.
	int fs_posixlock_op(const FsContext &context, inode_t inode, uint64_t start, uint64_t end,
	                    uint64_t owner, uint32_t sessionid, uint32_t reqid, uint32_t msgid,
	                    uint16_t oper, bool nonblocking,
	                    std::vector<FileLocks::Owner> &applied) override;

	/// Perform a POSIX lock probe on filesystem.
	/// @see IFilesystemOperations::fs_posixlock_probe.
	int fs_posixlock_probe(const FsContext &context, inode_t inode, uint64_t start, uint64_t end,
	                       uint64_t owner, uint32_t sessionid, uint32_t reqid, uint32_t msgid,
	                       uint16_t oper, safs_locks::FlockWrapper &info) override;

	/// Release (unlock + unqueue) all locks from a given session.
	/// @see IFilesystemOperations::fs_locks_clear_session.
	int fs_locks_clear_session(const FsContext &context, uint8_t type, inode_t inode,
	                           uint32_t sessionid, std::vector<FileLocks::Owner> &applied) override;

	/// List locks in the filesystem.
	/// @see IFilesystemOperations::fs_locks_list_all.
	int fs_locks_list_all(const FsContext &context, uint8_t type, bool pending, uint64_t start,
	                      uint64_t max, std::vector<safs_locks::Info> &outLocks) override;

	/// List locks for a specific inode.
	/// @see IFilesystemOperations::fs_locks_list_inode.
	int fs_locks_list_inode(const FsContext &context, uint8_t type, bool pending, inode_t inode,
	                        uint64_t start, uint64_t max,
	                        std::vector<safs_locks::Info> &outLocks) override;

	/// Unlocks the matching locks on the specified inode and tries to apply pending locks.
	/// @see IFilesystemOperations::fs_locks_unlock_inode.
	int fs_locks_unlock_inode(const FsContext &context, uint8_t type, inode_t inode,
	                          std::vector<FileLocks::Owner> &applied) override;

	/// Removes a pending lock matching the provided parameters.
	/// @see IFilesystemOperations::fs_locks_remove_pending.
	int fs_locks_remove_pending(const FsContext &context, uint8_t type, uint64_t ownerid,
	                            uint32_t sessionid, inode_t inode, uint64_t reqid) override;

private:
	/// Helper function used internally by `fs_flock_op` and `fs_posixlock_op`.
	static int fs_lock_op(const FsContext &context, FileLocks &locks, inode_t inode, uint64_t start,
	                      uint64_t end, uint64_t owner, uint32_t sessionid, uint32_t reqid,
	                      uint32_t msgid, uint16_t oper, bool nonblocking,
	                      std::vector<FileLocks::Owner> &applied);

	/// Helper function used internally by `fs_locks_unlock_inode`.
	static void fs_manage_lock_try_lock_pending(FileLocks &locks, inode_t inode, uint64_t start,
	                                            uint64_t end,
	                                            std::vector<FileLocks::Owner> &applied);
};

// Adds an entry to a changelog, updates filesystem.cc internal structures, prepends a
// proper timestamp to changelog entry and broadcasts it to metaloggers and shadow masters
void fs_changelog(uint32_t ts, const char *format, ...)
    __attribute__((__format__(__printf__, 2, 3)));
void fs_add_files_to_chunks(bool isMetadataLoading = true);

uint64_t fs_getversion();

uint8_t fs_full_path_by_inode(const FsContext &context, inode_t inode, std::string &fullPath);
