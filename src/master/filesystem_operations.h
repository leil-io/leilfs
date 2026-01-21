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
	/// Constructor
	/// @param _nodeOps Concrete node operations implementation.
	FilesystemOperationsBase(std::unique_ptr<IFilesystemNodeOperations> _nodeOps);

	/// Returns the concrete node operations implementation.
	IFilesystemNodeOperations *nodeOperations() override { return nodeOperations_.get(); }

	/// Creates a filesystem operation context for the specified transaction type.
	/// This implementation (in-memory) returns a context without transactions.
	/// @see IFilesystemOperations::createFilesystemOperationContext
	FilesystemOperationContext createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType type) override {
		(void)type;  // Unused parameter in this implementation
		return {};
	}

	/// Returns version of the loaded metadata.
	uint64_t getMetadataVersion() override;

	/// @see IFilesystemOperations::fs_changelog
	void changeLog(uint32_t ts, const char *format, ...) override
	    __attribute__((__format__(__printf__, 3, 4)));

	// Functions which create/apply (depending on the given context) changes to the metadata.
	// Common for metarestore and master server (both personalities)

	uint8_t acquire(const FsContext &context, inode_t inode, uint32_t sessionid) override;
	uint8_t append(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	               inode_t inode, inode_t inode_src) override;
	uint8_t deleteAcl(const FsContext &context, inode_t inode, AclType type) override;
	uint8_t link(const FsContext &context, inode_t inode_src, inode_t parent_dst,
	             const HString &name_dst, inode_t *inode, Attributes *attr) override;
	uint8_t purge(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	              inode_t inode) override;

	/// Renames (moves) a filesystem node from one location to another.
	/// @see IFilesystemOperations::rename
	uint8_t rename(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	               inode_t parent_src, const HString &name_src, inode_t parent_dst,
	               const HString &name_dst, inode_t *inode, Attributes *attr) override;

	uint8_t release(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                inode_t inode, uint32_t sessionid) override;
	uint8_t setExtraAttr(const FsContext &context, inode_t inode, uint8_t eattr, uint8_t smode,
	                     inode_t *sinodes, inode_t *ncinodes, inode_t *nsinodes) override;
	uint8_t setGoal(const FsContext &context, inode_t inode, uint8_t goal, uint8_t smode,
	                std::shared_ptr<SetGoalTask::StatsArray> setgoal_stats,
	                const std::function<void(int)> &callback) override;
	uint8_t applySetGoal(const FsContext &context, inode_t inode, uint8_t goal, uint8_t smode,
	                     uint32_t master_result) override;
	uint8_t setTrashPath(const FsContext &context, inode_t inode, const std::string &path) override;
	uint8_t setTrashTime(const FsContext &context, inode_t inode, uint32_t trashtime, uint8_t smode,
	                     std::shared_ptr<SetTrashtimeTask::StatsArray> settrashtime_stats,
	                     const std::function<void(int)> &callback) override;
	uint8_t applySetTrashTime(const FsContext &context, inode_t inode, uint32_t trashtime,
	                          uint8_t smode, uint32_t master_result) override;
	uint8_t symlink(const FsContext &context, inode_t parent, const HString &name,
	                const std::string &path, inode_t *inode, Attributes *attr) override;
	uint8_t undel(const FsContext &context, inode_t inode) override;
	uint8_t writeChunk(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                   inode_t inode, uint32_t index, bool usedummylockid,
	                   /* inout */ uint32_t *lockid, uint64_t *chunkid, uint8_t *opflag,
	                   uint64_t *length, uint32_t min_server_version = 0) override;
	uint8_t setNextChunkId(const FsContext &context, uint64_t nextChunkId) override;
	uint8_t getCanonicalPath(const FsContext &context,
	                         const FilesystemOperationContext &fsOpContext,
	                         const std::string &inputPath, std::string &canonicalPath) override;

#ifndef METARESTORE
	/// Returns a map with all defined goals.
	const std::map<int, Goal> &getAllGoalDefinitions() const override;

	/// Returns goal definition for given goal id.
	const Goal &getGoalDefinition(uint8_t goalId) const override;

	uint32_t reserveJobId() override;
	uint8_t cancelJob(uint32_t job_id) override;
	std::vector<JobInfo> getCurrentTasksInfo() override;

	uint8_t access(const FsContext &context, inode_t inode, int modemask) override;
	uint8_t lookup(const FsContext &context,
	               const FilesystemOperationContext &fsOpContext, inode_t parent, const HString &name,
	               inode_t *inode, Attributes &attr) override;
	uint8_t wholePathLookup(const FsContext &context, inode_t parent, const std::string &path,
	                        inode_t *found_inode, Attributes &attr) override;
	uint8_t getAttr(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                inode_t inode, Attributes &attr) override;
	uint8_t trySetLength(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                     inode_t inode, uint8_t opened, uint64_t length, bool denyTruncatingParity,
	                     uint32_t lockid, Attributes &attr, uint64_t *chunkid) override;
	uint8_t doSetLength(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                    inode_t inode, uint64_t length, Attributes &attr) override;
	uint8_t setAttr(const FsContext &context, inode_t inode, uint8_t setmask, uint16_t attrmode,
	                uint32_t attruid, uint32_t attrgid, uint32_t attratime, uint32_t attrmtime,
	                SugidClearMode sugidclearmode, Attributes &attr) override;
	uint8_t readlink(const FsContext &context, inode_t inode, std::string &path) override;
	void statfs(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	            uint64_t *totalspace, uint64_t *availspace, uint64_t *trashspace,
	            uint64_t *reservedspace, inode_t *inodes) override;
	uint8_t mknod(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	              inode_t parent, const HString &name, FSNodeType type, uint16_t mode,
	              uint16_t umask, uint32_t rdev, inode_t *inode, Attributes &attr) override;
	/// Creates a new directory in the filesystem.
	/// @see IFilesystemOperations::mkdir
	uint8_t mkdir(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	              inode_t parent, const HString &name, uint16_t mode, uint16_t umask,
	              uint8_t copysgid, inode_t *inode, Attributes &attr) override;
	uint8_t removeChunkFromFile(const FsContext &context,
	                            const FilesystemOperationContext &fsOpContext, inode_t inode,
	                            uint64_t chunkId) override;
	uint8_t repair(const FsContext &context, inode_t inode, uint8_t correct_only,
	               uint32_t *notchanged, uint32_t *erased, uint32_t *repaired) override;

	/// Removes an empty directory from the filesystem.
	/// @see IFilesystemOperations::rmdir
	uint8_t rmdir(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	              inode_t parent, const HString &name) override;

	uint8_t recursiveRemove(const FsContext &context, inode_t parent, const HString &name,
	                        const std::function<void(int)> &callback, uint32_t job_id) override;
	uint8_t readdirSize(const FsContext &context, inode_t inode, uint8_t flags, void **dnode,
	                    uint32_t *dbuffsize) override;
	void readdirData(const FsContext &context, uint8_t flags, void *dnode, uint8_t *dbuff) override;

	/// Reads a paginated list of entries from a directory.
	/// @see IFilesystemOperations::readdir
	uint8_t readdir(const FsContext &context, inode_t inode, uint64_t first_entry,
	                uint64_t number_of_entries, std::vector<DirectoryEntry> &dir_entries) override;

	uint8_t checkFile(const FsContext &context, inode_t inode,
	                  ChunkCountArray &chunkCount) override;
	uint8_t openCheck(const FsContext &context, inode_t inode, uint8_t flags,
	                  Attributes &attr) override;
	uint8_t getGoal(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                inode_t inode, uint8_t gmode, GoalStatistics &fgtab,
	                GoalStatistics &dgtab) override;
	uint8_t getXAttr(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                 inode_t inode, uint8_t opened, uint8_t anleng, const uint8_t *attrname,
	                 uint32_t *avleng, uint8_t **attrvalue) override;
	uint8_t getExtraAttr(const FsContext &context, inode_t inode, uint8_t gmode,
	                     ExtraAttributesArray &fileEAttrTab,
	                     ExtraAttributesArray &dirEAttrTab) override;
	uint8_t listXAttrLeng(const FsContext &context,
	                      const FilesystemOperationContext &fsOpContext, inode_t inode,
	                      uint8_t opened, void **xanode, uint32_t *xasize) override;
	uint8_t setXAttr(const FsContext &context, inode_t inode, uint8_t opened, uint8_t anleng,
	                 const uint8_t *attrname, uint32_t avleng, const uint8_t *attrvalue,
	                 uint8_t mode) override;

	/// Removes (unlinks) a file or non-directory node from the filesystem.
	/// @see IFilesystemOperations::unlink
	uint8_t unlink(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	               inode_t parent, const HString &name) override;

	uint8_t getChunksInfo(const FsContext &context, uint32_t current_ip, inode_t inode,
	                      uint32_t chunk_index, uint32_t chunk_count,
	                      std::vector<ChunkWithAddressAndLabel> &chunks) override;
	uint8_t getTrashTimePrepare(const FsContext &context, inode_t inode, uint8_t gmode,
	                            TrashtimeMap &fileTrashtimes, TrashtimeMap &dirTrashtimes) override;
	uint8_t setAcl(const FsContext &context, inode_t inode, AclType type,
	               const AccessControlList &acl) override;
	uint8_t setAcl(const FsContext &context, inode_t inode, const RichACL &acl) override;
	uint8_t getAcl(const FsContext &context, inode_t inode, RichACL &acl) override;

	// Functions which modify metadata or return some information.
	// To be used by the master server with personality == kMaster

	void getFSStats(uint64_t *totalSpace, uint64_t *availableSpace, uint64_t *trashSpace,
	                inode_t *trashNodes, uint64_t *reservedSpace, inode_t *reservedNodes,
	                inode_t *inodes, inode_t *directoryNodes, inode_t *fileNodes,
	                inode_t *linkNodes) override;
	uint32_t getDirPathSize(inode_t inode) override;
	void getDirPathData(inode_t inode, uint8_t *buff, uint32_t size) override;
	uint8_t getRootInode(inode_t *rootinode, const uint8_t *path) override;
	uint8_t endSetLength(uint64_t chunkid) override;
	uint8_t readChunk(inode_t inode, uint32_t indx, uint64_t *chunkid, uint64_t *length) override;
	uint8_t writeEnd(const FilesystemOperationContext &fsOpContext, inode_t inode, uint64_t length,
	                 uint64_t chunkid, uint32_t lockid) override;
	void getTrashTimeStore(TrashtimeMap &fileTrashtimes, TrashtimeMap &dirTrashtimes,
	                       uint8_t *buff) override;
	void listXAttrData(void *xanode, uint8_t *xabuff) override;

	uint32_t newSessionId() override;

	// RESERVED
	uint8_t readReservedSize(inode_t rootinode, uint8_t sesflags, uint32_t *dbuffsize) override;
	void readReservedData(inode_t rootinode, uint8_t sesflags, uint8_t *dbuff) override;
	void readReserved(uint32_t off, uint32_t max_entries,
	                  std::vector<NamedInodeEntry> &entries) override;
	void readReserved(uint64_t handleOffset, uint32_t maxEntries,
	                  std::vector<HandleInodeEntry> &entries) override;

	// TRASH
	uint8_t readTrashSize(inode_t rootinode, uint8_t sesflags, uint32_t *dbuffsize) override;
	void readTrashData(inode_t rootinode, uint8_t sesflags, uint8_t *dbuff) override;
	void readTrash(uint32_t off, uint32_t max_entries,
	               std::vector<NamedInodeEntry> &entries) override;
	void readTrash(uint64_t handleOffset, uint32_t maxEntries,
	               std::vector<HandleInodeEntry> &entries) override;
	uint8_t getTrashPath(inode_t rootinode, uint8_t sesflags, inode_t inode,
	                     std::string &path) override;

	// RESERVED+TRASH
	uint8_t getDetachedAttr(inode_t rootinode, uint8_t sesflags, inode_t inode, Attributes &attr,
	                        uint8_t dtype) override;

	// EXTRA
	uint8_t getDirStats(const FsContext &context, inode_t inode, inode_t *inodes, inode_t *dirs,
	                    inode_t *files, inode_t *links, uint32_t *chunks, uint64_t *length,
	                    uint64_t *size, uint64_t *rsize) override;
	uint8_t getChunkId(const FsContext &context,
	                   const FilesystemOperationContext &fsOpContext, inode_t inode, uint32_t index,
	                   uint64_t *chunkid) override;

	// SPECIAL - LOG EMERGENCY INCREASE VERSION FROM CHUNKS-MODULE
	void increaseChunkVersion(uint64_t chunkid) override;

	uint8_t fullPathByInode(const FsContext &context, inode_t inode,
	                        std::string &fullPath) override;
	std::string fullPathByInode(inode_t initialInode) override;

	// QUOTAS

	uint8_t quotaGetAll(const FsContext &context, std::vector<QuotaEntry> &results) override;
	uint8_t quotaGet(const FsContext &context, const std::vector<QuotaOwner> &owners,
	                 std::vector<QuotaEntry> &results) override;
	uint8_t quotaSet(const FsContext &context, const std::vector<QuotaEntry> &entries) override;
	uint8_t quotaGetInfo(const FsContext &context, const std::vector<QuotaEntry> &entries,
	                     std::vector<std::string> &result) override;

	// CHECKSUM

	/// Starts recalculating metadata checksum in background.
	/// @see IFilesystemOperations::fs_start_checksum_recalculation.
	uint8_t startChecksumRecalculation() override;

#endif
	void addFilesToChunks(bool isMetadataLoading = true) override;

	// Functions which apply changes from changelog, only for shadow master and metarestore
	uint8_t applyChecksum(const std::string &version, uint64_t checksum) override;
	uint8_t applyCreate(uint32_t timestamp, inode_t parent, const HString &name, FSNodeType type,
	                    uint32_t mode, uint32_t uid, uint32_t gid, uint32_t rdev,
	                    inode_t inode) override;
	uint8_t applyAccess(uint32_t timestamp, inode_t inode) override;
	uint8_t applyAttr(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                  inode_t inode, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t atime,
	                  uint32_t mtime) override;
	uint8_t applySession(uint32_t sessionid) override;
	uint8_t applyIncreaseChunkVersion(uint64_t chunkid) override;
	uint8_t applyLength(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                    inode_t inode, uint64_t length, bool eraseFurtherChunks) override;
	uint8_t applyRepair(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                    inode_t inode, uint32_t indx, uint32_t nversion) override;
	uint8_t applySetXAttr(uint32_t timestamp, inode_t inode, uint32_t anleng,
	                      const uint8_t *attrname, uint32_t avleng, const uint8_t *attrvalue,
	                      uint32_t mode) override;
	uint8_t applySetAcl(uint32_t timestamp, inode_t inode, char aclType,
	                    const char *aclString) override;
	uint8_t applySetRichAcl(uint32_t timestamp, inode_t inode,
	                        const std::string &acl_string) override;
	uint8_t applyUnlink(uint32_t timestamp, inode_t parent, const HString &name,
	                    inode_t inode) override;
	uint8_t applyUnlock(uint64_t chunkid) override;
	uint8_t applyTrunc(uint32_t timestamp, inode_t inode, uint32_t indx, uint64_t chunkid,
	                   uint32_t lockid) override;

	uint8_t applySetQuota(char rigor, char resource, char ownerType, inode_t ownerId,
	                      uint64_t limit) override;

	// CHECKSUM

	/// Returns checksum of the loaded metadata.
	uint64_t metadataChecksum(ChecksumMode mode) override;

	// Locks

	/// Perform a flock operation on filesystem.
	/// @see IFilesystemOperations::fs_flock_op.
	int flockOperation(const FsContext &context, inode_t inode, uint64_t owner, uint32_t sessionid,
	                   uint32_t reqid, uint32_t msgid, uint16_t oper, bool nonblocking,
	                   std::vector<FileLocks::Owner> &applied) override;

	/// Perform a posix lock operation on filesystem.
	/// @see IFilesystemOperations::fs_posixlock_op.
	int posixLockOperation(const FsContext &context, inode_t inode, uint64_t start, uint64_t end,
	                       uint64_t owner, uint32_t sessionid, uint32_t reqid, uint32_t msgid,
	                       uint16_t oper, bool nonblocking,
	                       std::vector<FileLocks::Owner> &applied) override;

	/// Perform a POSIX lock probe on filesystem.
	/// @see IFilesystemOperations::fs_posixlock_probe.
	int posixLockProbe(const FsContext &context, inode_t inode, uint64_t start, uint64_t end,
	                   uint64_t owner, uint32_t sessionid, uint32_t reqid, uint32_t msgid,
	                   uint16_t oper, safs_locks::FlockWrapper &info) override;

	/// Release (unlock + unqueue) all locks from a given session.
	/// @see IFilesystemOperations::fs_locks_clear_session.
	int locksClearSession(const FsContext &context, uint8_t type, inode_t inode, uint32_t sessionid,
	                      std::vector<FileLocks::Owner> &applied) override;

	/// List locks in the filesystem.
	/// @see IFilesystemOperations::fs_locks_list_all.
	int locksListAll(const FsContext &context, uint8_t type, bool pending, uint64_t start,
	                 uint64_t max, std::vector<safs_locks::Info> &outLocks) override;

	/// List locks for a specific inode.
	/// @see IFilesystemOperations::fs_locks_list_inode.
	int locksListInode(const FsContext &context, uint8_t type, bool pending, inode_t inode,
	                   uint64_t start, uint64_t max,
	                   std::vector<safs_locks::Info> &outLocks) override;

	/// Unlocks the matching locks on the specified inode and tries to apply pending locks.
	/// @see IFilesystemOperations::fs_locks_unlock_inode.
	int locksUnlockInode(const FsContext &context, uint8_t type, inode_t inode,
	                     std::vector<FileLocks::Owner> &applied) override;

	/// Removes a pending lock matching the provided parameters.
	/// @see IFilesystemOperations::fs_locks_remove_pending.
	int locksRemovePending(const FsContext &context, uint8_t type, uint64_t ownerid,
	                       uint32_t sessionid, inode_t inode, uint64_t reqid) override;

private:
	/// Helper function used internally by `fs_flock_op` and `fs_posixlock_op`.
	static int lockOperation(const FsContext &context, FileLocks &locks, inode_t inode,
	                         uint64_t start, uint64_t end, uint64_t owner, uint32_t sessionid,
	                         uint32_t reqid, uint32_t msgid, uint16_t oper, bool nonblocking,
	                         std::vector<FileLocks::Owner> &applied);

	/// Helper function used internally by `fs_locks_unlock_inode`.
	static void manageLockTryLockPending(FileLocks &locks, inode_t inode, uint64_t start,
	                                     uint64_t end, std::vector<FileLocks::Owner> &applied);

	/// Node operations object
	std::unique_ptr<IFilesystemNodeOperations> nodeOperations_;
};
