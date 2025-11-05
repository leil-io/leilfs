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

#include "common/attributes.h"
#include "common/goal.h"
#include "master/checksum.h"
#include "master/filesystem_node_operations_interface.h"
#include "master/filesystem_node_types.h"
#include "master/fs_context.h"
#include "master/locks.h"
#include "master/setgoal_task.h"
#include "master/settrashtime_task.h"

class HString;
class AccessControlList;
class RichACL;

struct DirectoryEntry;
struct ChunkWithAddressAndLabel;

struct QuotaEntry;
struct QuotaOwner;

struct NamedInodeEntry;
struct HandleInodeEntry;

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

	/// Returns the concrete node operations implementation.
	virtual IFilesystemNodeOperations *nodeOperations() = 0;

	/// Returns version of the loaded metadata.
	virtual uint64_t getMetadataVersion() = 0;

	/// Adds an entry to a changelog, updates filesystem.cc internal structures, prepends a
	/// proper timestamp to changelog entry and broadcasts it to metaloggers and shadow masters.
	/// The attribute is used to ensure printf-like format string checking by the compiler.
	virtual void changeLog(uint32_t ts, const char *format, ...)
	    __attribute__((__format__(__printf__, 3, 4))) = 0;

	// Functions which create/apply (depending on the given context) changes to the metadata.
	// Common for metarestore and master server (both personalities)

	virtual uint8_t acquire(const FsContext &context, inode_t inode, uint32_t sessionid) = 0;
	virtual uint8_t append(const FsContext &context, inode_t inode, inode_t inode_src) = 0;
	virtual uint8_t deleteAcl(const FsContext &context, inode_t inode, AclType type) = 0;
	virtual uint8_t link(const FsContext &context, inode_t inode_src, inode_t parent_dst,
	                     const HString &name_dst, inode_t *inode, Attributes *attr) = 0;
	virtual uint8_t purge(const FsContext &context, inode_t inode) = 0;
	virtual uint8_t rename(const FsContext &context, inode_t parent_src, const HString &name_src,
	                       inode_t parent_dst, const HString &name_dst, inode_t *inode,
	                       Attributes *attr) = 0;
	virtual uint8_t release(const FsContext &context, inode_t inode, uint32_t sessionid) = 0;
	virtual uint8_t setEAttr(const FsContext &context, inode_t inode, uint8_t eattr, uint8_t smode,
	                         inode_t *sinodes, inode_t *ncinodes, inode_t *nsinodes) = 0;
	virtual uint8_t setGoal(const FsContext &context, inode_t inode, uint8_t goal, uint8_t smode,
	                        std::shared_ptr<SetGoalTask::StatsArray> setgoal_stats,
	                        const std::function<void(int)> &callback) = 0;
	virtual uint8_t applySetGoal(const FsContext &context, inode_t inode, uint8_t goal,
	                             uint8_t smode, uint32_t master_result) = 0;
	virtual uint8_t setTrashPath(const FsContext &context, inode_t inode,
	                             const std::string &path) = 0;
	virtual uint8_t setTrashTime(const FsContext &context, inode_t inode, uint32_t trashtime,
	                             uint8_t smode,
	                             std::shared_ptr<SetTrashtimeTask::StatsArray> settrashtime_stats,
	                             const std::function<void(int)> &callback) = 0;
	virtual uint8_t applySetTrashTime(const FsContext &context, inode_t inode, uint32_t trashtime,
	                                  uint8_t smode, uint32_t master_result) = 0;
	virtual uint8_t symlink(const FsContext &context, inode_t parent, const HString &name,
	                        const std::string &path, inode_t *inode, Attributes *attr) = 0;
	virtual uint8_t undel(const FsContext &context, inode_t inode) = 0;
	virtual uint8_t writeChunk(const FsContext &context, inode_t inode, uint32_t index,
	                           bool usedummylockid,
	                           /* inout */ uint32_t *lockid, uint64_t *chunkid, uint8_t *opflag,
	                           uint64_t *length, uint32_t min_server_version = 0) = 0;
	virtual uint8_t setNextChunkId(const FsContext &context, uint64_t nextChunkId) = 0;

#ifndef METARESTORE
	/// Returns a map with all defined goals.
	virtual const std::map<int, Goal> &getAllGoalDefinitions() const = 0;

	/// Returns goal definition for given goal id.
	virtual const Goal &getGoalDefinition(uint8_t goalId) const = 0;

	virtual uint32_t reserveJobId() = 0;
	virtual uint8_t cancelJob(uint32_t job_id) = 0;
	/// Return info about currently executed tasks
	virtual std::vector<JobInfo> getCurrentTasksInfo() = 0;

	virtual uint8_t access(const FsContext &context, inode_t inode, int modemask) = 0;
	virtual uint8_t lookup(const FsContext &context, inode_t parent, const HString &name,
	                       inode_t *inode, Attributes &attr) = 0;
	virtual uint8_t wholePathLookup(const FsContext &context, inode_t parent,
	                                const std::string &path, inode_t *found_inode,
	                                Attributes &attr) = 0;
	virtual uint8_t getAttr(const FsContext &context, inode_t inode, Attributes &attr) = 0;
	virtual uint8_t trySetLength(const FsContext &context, inode_t inode, uint8_t opened,
	                             uint64_t length, bool denyTruncatingParity, uint32_t lockid,
	                             Attributes &attr, uint64_t *chunkid) = 0;
	virtual uint8_t doSetLength(const FsContext &context, inode_t inode, uint64_t length,
	                            Attributes &attr) = 0;
	virtual uint8_t setAttr(const FsContext &context, inode_t inode, uint8_t setmask,
	                        uint16_t attrmode, uint32_t attruid, uint32_t attrgid,
	                        uint32_t attratime, uint32_t attrmtime, SugidClearMode sugidclearmode,
	                        Attributes &attr) = 0;
	virtual uint8_t readlink(const FsContext &context, inode_t inode, std::string &path) = 0;
	virtual void statfs(const FsContext &context, uint64_t *totalspace, uint64_t *availspace,
	                    uint64_t *trashspace, uint64_t *reservedspace, inode_t *inodes) = 0;
	virtual uint8_t mknod(const FsContext &context, inode_t parent, const HString &name,
	                      FSNodeType type, uint16_t mode, uint16_t umask, uint32_t rdev,
	                      inode_t *inode, Attributes &attr) = 0;
	virtual uint8_t mkdir(const FsContext &context, inode_t parent, const HString &name,
	                      uint16_t mode, uint16_t umask, uint8_t copysgid, inode_t *inode,
	                      Attributes &attr) = 0;
	virtual uint8_t removeChunkFromFile(const FsContext &context, inode_t inode,
	                                    uint64_t chunkId) = 0;
	virtual uint8_t repair(const FsContext &context, inode_t inode, uint8_t correct_only,
	                       uint32_t *notchanged, uint32_t *erased, uint32_t *repaired) = 0;
	virtual uint8_t rmdir(const FsContext &context, inode_t parent, const HString &name) = 0;
	virtual uint8_t recursiveRemove(const FsContext &context, inode_t parent, const HString &name,
	                                const std::function<void(int)> &callback, uint32_t job_id) = 0;
	virtual uint8_t readdirSize(const FsContext &context, inode_t inode, uint8_t flags,
	                            void **dnode, uint32_t *dbuffsize) = 0;
	virtual void readdirData(const FsContext &context, uint8_t flags, void *dnode,
	                         uint8_t *dbuff) = 0;

	virtual uint8_t readdir(const FsContext &context, inode_t inode, uint64_t first_entry,
	                        uint64_t number_of_entries,
	                        std::vector<DirectoryEntry> &dir_entries) = 0;

	virtual uint8_t checkFile(const FsContext &context, inode_t inode,
	                          uint32_t chunkcount[CHUNK_MATRIX_SIZE]) = 0;
	virtual uint8_t openCheck(const FsContext &context, inode_t inode, uint8_t flags,
	                          Attributes &attr) = 0;
	virtual uint8_t getGoal(const FsContext &context, inode_t inode, uint8_t gmode,
	                        GoalStatistics &fgtab, GoalStatistics &dgtab) = 0;
	virtual uint8_t getEAttr(const FsContext &context, inode_t inode, uint8_t gmode,
	                         uint32_t feattrtab[16], uint32_t deattrtab[16]) = 0;
	virtual uint8_t listXAttrLeng(const FsContext &context, inode_t inode, uint8_t opened,
	                              void **xanode, uint32_t *xasize) = 0;
	virtual uint8_t getXAttr(const FsContext &context, inode_t inode, uint8_t opened,
	                         uint8_t anleng, const uint8_t *attrname, uint32_t *avleng,
	                         uint8_t **attrvalue) = 0;
	virtual uint8_t setXAttr(const FsContext &context, inode_t inode, uint8_t opened,
	                         uint8_t anleng, const uint8_t *attrname, uint32_t avleng,
	                         const uint8_t *attrvalue, uint8_t mode) = 0;
	virtual uint8_t unlink(const FsContext &context, inode_t parent, const HString &name) = 0;
	virtual uint8_t getChunksInfo(const FsContext &context, uint32_t current_ip, inode_t inode,
	                              uint32_t chunk_index, uint32_t chunk_count,
	                              std::vector<ChunkWithAddressAndLabel> &chunks) = 0;
	virtual uint8_t getTrashTimePrepare(const FsContext &context, inode_t inode, uint8_t gmode,
	                                    TrashtimeMap &fileTrashtimes,
	                                    TrashtimeMap &dirTrashtimes) = 0;
	virtual uint8_t setAcl(const FsContext &context, inode_t inode, AclType type,
	                       const AccessControlList &acl) = 0;
	virtual uint8_t setAcl(const FsContext &context, inode_t inode, const RichACL &acl) = 0;
	virtual uint8_t getAcl(const FsContext &context, inode_t inode, RichACL &acl) = 0;

	// Functions which modify metadata or return some information.
	// To be used by the master server with personality == kMaster

	virtual void getFSStats(uint64_t *totalSpace, uint64_t *availableSpace, uint64_t *trashSpace,
	                        inode_t *trashNodes, uint64_t *reservedSpace, inode_t *reservedNodes,
	                        inode_t *inodes, inode_t *directoryNodes, inode_t *fileNodes,
	                        inode_t *linkNodes) = 0;
	virtual uint32_t getDirPathSize(inode_t inode) = 0;
	virtual void getDirPathData(inode_t inode, uint8_t *buff, uint32_t size) = 0;
	virtual uint8_t getRootInode(inode_t *rootinode, const uint8_t *path) = 0;
	virtual uint8_t endSetLength(uint64_t chunkid) = 0;
	virtual uint8_t readChunk(inode_t inode, uint32_t indx, uint64_t *chunkid,
	                          uint64_t *length) = 0;
	virtual uint8_t writeEnd(inode_t inode, uint64_t length, uint64_t chunkid, uint32_t lockid) = 0;
	virtual void getTrashTimeStore(TrashtimeMap &fileTrashtimes, TrashtimeMap &dirTrashtimes,
	                               uint8_t *buff) = 0;
	virtual void listXAttrData(void *xanode, uint8_t *xabuff) = 0;

	virtual uint32_t newSessionId() = 0;

	// RESERVED
	virtual uint8_t readReservedSize(inode_t rootinode, uint8_t sesflags, uint32_t *dbuffsize) = 0;
	virtual void readReservedData(inode_t rootinode, uint8_t sesflags, uint8_t *dbuff) = 0;
	virtual void readReserved(uint32_t off, uint32_t max_entries,
	                          std::vector<NamedInodeEntry> &entries) = 0;
	virtual void readReserved(uint64_t handleOffset, uint32_t maxEntries,
	                          std::vector<HandleInodeEntry> &entries) = 0;

	// TRASH
	virtual uint8_t readTrashSize(inode_t rootinode, uint8_t sesflags, uint32_t *dbuffsize) = 0;
	virtual void readTrashData(inode_t rootinode, uint8_t sesflags, uint8_t *dbuff) = 0;
	virtual void readTrash(uint32_t off, uint32_t max_entries,
	                       std::vector<NamedInodeEntry> &entries) = 0;
	virtual void readTrash(uint64_t handleOffset, uint32_t maxEntries,
	                       std::vector<HandleInodeEntry> &entries) = 0;
	virtual uint8_t getTrashPath(inode_t rootinode, uint8_t sesflags, inode_t inode,
	                             std::string &path) = 0;

	// RESERVED+TRASH
	virtual uint8_t getDetachedAttr(inode_t rootinode, uint8_t sesflags, inode_t inode,
	                                Attributes &attr, uint8_t dtype) = 0;

	// EXTRA
	virtual uint8_t getDirStats(const FsContext &context, inode_t inode, inode_t *inodes,
	                            inode_t *dirs, inode_t *files, inode_t *links, uint32_t *chunks,
	                            uint64_t *length, uint64_t *size, uint64_t *rsize) = 0;
	virtual uint8_t getChunkId(const FsContext &context, inode_t inode, uint32_t index,
	                           uint64_t *chunkid) = 0;

	// SPECIAL - LOG EMERGENCY INCREASE VERSION FROM CHUNKS-MODULE
	virtual void increaseChunkVersion(uint64_t chunkid) = 0;

	virtual uint8_t fullPathByInode(const FsContext &context, inode_t inode,
	                                std::string &fullPath) = 0;
	virtual std::string fullPathByInode(inode_t initial_inode) = 0;

	// QUOTAS

	virtual uint8_t quotaGetAll(const FsContext &context, std::vector<QuotaEntry> &results) = 0;
	virtual uint8_t quotaGet(const FsContext &context, const std::vector<QuotaOwner> &owners,
	                         std::vector<QuotaEntry> &results) = 0;
	virtual uint8_t quotaSet(const FsContext &context, const std::vector<QuotaEntry> &entries) = 0;
	virtual uint8_t quotaGetInfo(const FsContext &context, const std::vector<QuotaEntry> &entries,
	                             std::vector<std::string> &result) = 0;

	// CHECKSUM

	/// Starts recalculating metadata checksum in background.
	/// @return SAUNAFS_STATUS_OK if dump started successfully, otherwise cause of the failure.
	virtual uint8_t startChecksumRecalculation() = 0;
#endif
	virtual void addFilesToChunks(bool isMetadataLoading = true) = 0;

	// Functions which apply changes from changelog, only for shadow master and metarestore
	virtual uint8_t applyChecksum(const std::string &version, uint64_t checksum) = 0;
	virtual uint8_t applyCreate(uint32_t timestamp, inode_t parent, const HString &name,
	                            FSNodeType type, uint32_t mode, uint32_t uid, uint32_t gid,
	                            uint32_t rdev, inode_t inode) = 0;
	virtual uint8_t applyAccess(uint32_t timestamp, inode_t inode) = 0;
	virtual uint8_t applyAttr(uint32_t timestamp, inode_t inode, uint32_t mode, uint32_t uid,
	                          uint32_t gid, uint32_t atime, uint32_t mtime) = 0;
	virtual uint8_t applySession(uint32_t sessionid) = 0;
	virtual uint8_t applyIncreaseChunkVersion(uint64_t chunkid) = 0;
	virtual uint8_t applyLength(uint32_t timestamp, inode_t inode, uint64_t length,
	                            bool eraseFurtherChunks) = 0;
	virtual uint8_t applyRepair(uint32_t timestamp, inode_t inode, uint32_t indx,
	                            uint32_t nversion) = 0;
	virtual uint8_t applySetXAttr(uint32_t timestamp, inode_t inode, uint32_t anleng,
	                              const uint8_t *attrname, uint32_t avleng,
	                              const uint8_t *attrvalue, uint32_t mode) = 0;
	virtual uint8_t applySetAcl(uint32_t timestamp, inode_t inode, char aclType,
	                            const char *aclString) = 0;
	virtual uint8_t applySetRichAcl(uint32_t timestamp, inode_t inode,
	                                const std::string &acl_string) = 0;
	virtual uint8_t applyUnlink(uint32_t timestamp, inode_t parent, const HString &name,
	                            inode_t inode) = 0;
	virtual uint8_t applyUnlock(uint64_t chunkid) = 0;
	virtual uint8_t applyTrunc(uint32_t timestamp, inode_t inode, uint32_t indx, uint64_t chunkid,
	                           uint32_t lockid) = 0;

	virtual uint8_t applySetQuota(char rigor, char resource, char ownerType, inode_t ownerId,
	                              uint64_t limit) = 0;

	// CHECKSUM

	/// Returns checksum of the loaded metadata.
	virtual uint64_t metadataChecksum(ChecksumMode mode) = 0;

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
	virtual int flockOperation(const FsContext &context, inode_t inode, uint64_t owner,
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
	virtual int posixLockOperation(const FsContext &context, inode_t inode, uint64_t start,
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
	virtual int posixLockProbe(const FsContext &context, inode_t inode, uint64_t start,
	                           uint64_t end, uint64_t owner, uint32_t sessionid, uint32_t reqid,
	                           uint32_t msgid, uint16_t oper, safs_locks::FlockWrapper &info) = 0;

	/// Release (unlock + unqueue) all locks from a given session.
	/// @param context Filesystem context.
	/// @param type Type of locks to clear (kFlock, kPosix).
	/// @param inode inode number on which to clear locks.
	/// @param sessionid Session id whose locks are to be cleared.
	/// @param applied Vector to be filled with the owners of the cleared locks.
	virtual int locksClearSession(const FsContext &context, uint8_t type, inode_t inode,
	                              uint32_t sessionid, std::vector<FileLocks::Owner> &applied) = 0;

	/// List locks in the filesystem.
	/// Fills outLocks with locks matching the type and pending parameters.
	/// @param context Filesystem context (could be ignored in some implementations).
	/// @param type Type of locks to list (kFlock, kPosix).
	/// @param pending If true, lists pending locks, otherwise lists active locks.
	/// @param start Start index for listing.
	/// @param max Maximum number of locks to list.
	/// @param outLocks Vector to be filled with the listed locks.
	virtual int locksListAll(const FsContext &context, uint8_t type, bool pending, uint64_t start,
	                         uint64_t max, std::vector<safs_locks::Info> &outLocks) = 0;

	/// List locks for a specific inode.
	/// @param context Filesystem context (could be ignored in some implementations).
	/// @param type Type of locks to list (kFlock, kPosix).
	/// @param pending If true, lists pending locks, otherwise lists active locks.
	/// @param inode inode number on which to list locks.
	/// @param start Start index for listing.
	/// @param max Maximum number of locks to list.
	/// @param outLocks Vector to be filled with the listed locks.
	virtual int locksListInode(const FsContext &context, uint8_t type, bool pending, inode_t inode,
	                           uint64_t start, uint64_t max,
	                           std::vector<safs_locks::Info> &outLocks) = 0;

	/// Unlocks the matching locks on the specified inode and tries to apply pending locks.
	/// @param context Filesystem context.
	/// @param type Type of locks to unlock (kFlock, kPosix).
	/// @param inode inode number on which to unlock locks.
	/// @param applied Vector to be filled with the owners of the unlocked locks.
	virtual int locksUnlockInode(const FsContext &context, uint8_t type, inode_t inode,
	                             std::vector<FileLocks::Owner> &applied) = 0;

	/// Removes a pending lock matching the provided parameters.
	/// @param context Filesystem context.
	/// @param type Type of lock to operate on (kFlock, kPosix).
	/// @param ownerid Owner identifier provided by the client (FUSE owner typically).
	/// @param sessionid Session id of the client that enqueued the lock.
	/// @param inode Inode number on which the pending lock was queued.
	/// @param reqid Request id (used to identify interruptible requests).
	virtual int locksRemovePending(const FsContext &context, uint8_t type, uint64_t ownerid,
	                               uint32_t sessionid, inode_t inode, uint64_t reqid) = 0;
};

// Global filesystem operations instance.
// This global unique_ptr is initialized once at startup (before any FS calls) and set to a single
// concrete implementation for the process lifetime. It must not be reassigned and its dynamic type
// remains stable, so callers may assume one immutable implementation.
inline std::unique_ptr<IFilesystemOperations> gFSOperations = nullptr;
