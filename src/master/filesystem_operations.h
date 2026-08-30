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
#include <optional>

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

	TaskManager::Task *createSnapshotTask(SnapshotTask::SubtaskContainer &&subtasks,
	                                      inode_t originalInode, inode_t destinationParentInode,
	                                      inode_t destinationInode, uint8_t canOverwrite,
	                                      uint8_t ignoreMissingSource, bool emitChangelog,
	                                      bool enqueueWork) override {
		return new SnapshotTask(std::move(subtasks), originalInode, destinationParentInode,
		                        destinationInode, canOverwrite, ignoreMissingSource, emitChangelog,
		                        enqueueWork);
	}

	/// Returns version of the loaded metadata.
	uint64_t getMetadataVersion() override;

	/// Increments the metadata version counter and returns the pre-increment value (the version of
	/// this mutation).
	/// @see IFilesystemOperations::increaseMetadataVersion
	uint64_t increaseMetadataVersion(const FilesystemOperationContext &fsOpContext) override;

	/// @see IFilesystemOperations::fs_changelog
	void changeLog(const FilesystemOperationContext &fsOpContext, uint32_t ts, const char *format,
	               ...) override __attribute__((__format__(__printf__, 4, 5)));

	// Functions which create/apply (depending on the given context) changes to the metadata.
	// Common for metarestore and master server (both personalities)

	/// Registers a session as having acquired (opened) a file.
	/// @see IFilesystemOperations::acquire
	uint8_t acquire(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                inode_t inode, uint32_t sessionid) override;

	/// Registers a session as having released (closed) a file.
	/// @see IFilesystemOperations::release
	uint8_t release(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                inode_t inode, uint32_t sessionid) override;

	/// Appends the contents of one file to another.
	/// @see IFilesystemOperations::append
	uint8_t append(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	               inode_t inode, inode_t inode_src) override;
	uint8_t appendAsync(const FsContext &context, inode_t inode, inode_t sourceInode,
	                    const std::function<void(int)> &callback) override;

	/// Removes or prunes the ACL stored on a node, given its inode.
	/// @see IFilesystemOperations::deleteAcl
	uint8_t deleteAcl(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                  inode_t inode, AclType type) override;

	/// Creates a hard link to an existing file in a destination directory.
	/// @see IFilesystemOperations::link
	uint8_t link(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	             inode_t inode_src, inode_t parent_dst, const HString &name_dst, inode_t *inode,
	             Attributes *attr) override;

	/// Purges a trash node from metadata.
	/// @see IFilesystemOperations::purge
	uint8_t purge(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	              inode_t inode) override;

	/// Renames (moves) a filesystem node from one location to another.
	/// @see IFilesystemOperations::rename
	uint8_t rename(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	               inode_t parent_src, const HString &name_src, inode_t parent_dst,
	               const HString &name_dst, inode_t *inode, Attributes *attr) override;

	/// Updates extra-attribute flags on a node (optionally recursively).
	/// @see IFilesystemOperations::setExtraAttr
	uint8_t setExtraAttr(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                     inode_t inode, uint8_t eattr, uint8_t smode, inode_t *sinodes,
	                     inode_t *ncinodes, inode_t *nsinodes) override;

	/// Schedules setting a storage goal on a node (optionally recursively).
	/// @see IFilesystemOperations::setGoal
	uint8_t setGoal(const FsContext &context, inode_t inode, uint8_t goal, uint8_t smode,
	                std::shared_ptr<SetGoalTask::StatsArray> setgoal_stats,
	                const std::function<void(int)> &callback) override;

	/// Applies a single-node goal update during shadow/restore replay and verifies consistency.
	/// @see IFilesystemOperations::applySetGoal
	uint8_t applySetGoal(const FsContext &context, inode_t inode, uint8_t goal, uint8_t smode,
	                     uint32_t master_result) override;

	/// Updates the stored path string of a trash inode.
	/// @see IFilesystemOperations::setTrashPath
	uint8_t setTrashPath(const FsContext &context, inode_t inode, const std::string &path) override;

	/// Schedules setting trash-time on a node (optionally recursively).
	/// @see IFilesystemOperations::setTrashTime
	uint8_t setTrashTime(const FsContext &context, inode_t inode, uint32_t trashtime, uint8_t smode,
	                     std::shared_ptr<SetTrashtimeTask::StatsArray> settrashtime_stats,
	                     const std::function<void(int)> &callback) override;

	/// Applies a single-node trash-time update on shadow/restore replay and verifies consistency.
	/// @see IFilesystemOperations::applySetTrashTime
	uint8_t applySetTrashTime(const FsContext &context, inode_t inode, uint32_t trashtime,
	                          uint8_t smode, uint32_t master_result) override;

	/// Creates a symbolic link (symlink) in the filesystem.
	/// @see IFilesystemOperations::symlink
	uint8_t symlink(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                inode_t parent, const HString &name, const std::string &path, inode_t *inode,
	                Attributes *attr) override;

	/// Restores a file from trash using the path stored with its detached metadata.
	/// @see IFilesystemOperations::undel
	uint8_t undel(const FsContext &context, inode_t inode) override;

	/// Prepares a file chunk for client-side writing.
	/// @see IFilesystemOperations::writeChunk
	uint8_t writeChunk(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                   inode_t inode, uint32_t index,
	                   /* inout */ uint32_t *lockid, uint64_t *chunkid, uint8_t *opflag,
	                   uint64_t *length, [[maybe_unused]] uint32_t min_server_version = 0,
	                   uint64_t opNonce = 0, uint64_t requestedGrantRandom = 0,
	                   uint64_t *grantGeneration = nullptr,
	                   uint64_t *grantRandom = nullptr) override;

	/// Raises the next chunk-id floor used by strictly monotonic chunk-id generators.
	/// @see IFilesystemOperations::setNextChunkId
	uint8_t setNextChunkId(const FsContext &context, uint64_t nextChunkId) override;

	uint8_t getCanonicalPath(const FsContext &context,
	                         const FilesystemOperationContext &fsOpContext,
	                         const std::string &inputPath, std::string &canonicalPath) override;

#ifndef METARESTORE
	/// Updates the configured duration of one full file-test pass.
	static void setFileTestLoopTime(uint32_t loopTime);

	/// Returns the configured duration of one full file-test pass.
	static uint32_t fileTestLoopTime();

	/// Returns a map with all defined goals.
	const std::map<int, Goal> &getAllGoalDefinitions() const override;

	/// Returns goal definition for given goal id.
	const Goal &getGoalDefinition(uint8_t goalId) const override;

	uint32_t reserveJobId() override;
	uint8_t cancelJob(uint32_t job_id) override;
	std::vector<JobInfo> getCurrentTasksInfo() override;

	/// Checks whether the client session can access the file specified by inode.
	/// @see IFilesystemOperations::access
	uint8_t access(const FsContext &context, inode_t inode, int modemask) override;

	/// Looks up a child entry in a directory by name.
	/// @see IFilesystemOperations::lookup
	uint8_t lookup(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	               inode_t parent, const HString &name, inode_t *inode, Attributes &attr) override;

	/// Looks up a node by traversing a multi-component path.
	/// @see IFilesystemOperations::wholePathLookup
	uint8_t wholePathLookup(const FsContext &context, inode_t parent, const std::string &path,
	                        inode_t *found_inode, Attributes &attr) override;

	/// Retrieves the attributes of a filesystem node.
	/// @see IFilesystemOperations::getAttr
	uint8_t getAttr(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                inode_t inode, Attributes &attr) override;

	/// Attempts to initiate a file truncate operation (phase 1).
	/// @see IFilesystemOperations::trySetLength
	uint8_t trySetLength(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                     inode_t inode, uint8_t opened, uint64_t length, bool denyTruncatingParity,
	                     uint32_t lockid, Attributes &attr, uint64_t *chunkid) override;

	/// Finalizes a file truncate operation by setting the file length metadata (phase 2).
	/// @see IFilesystemOperations::doSetLength
	uint8_t doSetLength(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                    inode_t inode, uint64_t length, Attributes &attr) override;

	/// Unlocks a chunk after a truncate operation completes (phase 1.5).
	/// @see IFilesystemOperations::endSetLength
	uint8_t endSetLength(const FilesystemOperationContext &fsOpContext, uint64_t chunkid) override;

	/// Sets attributes for a filesystem node (file, directory, etc.).
	/// @see IFilesystemOperations::setAttr
	uint8_t setAttr(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                inode_t inode, uint8_t setmask, uint16_t attrmode, uint32_t attruid,
	                uint32_t attrgid, uint32_t attratime, uint32_t attrmtime,
	                SugidClearMode sugidclearmode, Attributes &attr) override;

	/// Reads the target path stored in a symbolic link.
	/// @see IFilesystemOperations::readlink
	uint8_t readlink(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                 inode_t inode, std::string &path) override;

	/// Reports filesystem space and inode statistics visible from the session root.
	/// @see IFilesystemOperations::statfs
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

	/// Removes one chunk reference from a file-like node by chunk id.
	/// @see IFilesystemOperations::removeChunkFromFile
	uint8_t removeChunkFromFile(const FsContext &context,
	                            const FilesystemOperationContext &fsOpContext, inode_t inode,
	                            uint32_t chunkIndex, uint64_t chunkId) override;

	/// Scans and repairs chunk metadata for a file-like node.
	/// @see IFilesystemOperations::repair
	uint8_t repair(const FsContext &context, inode_t inode, uint8_t correct_only,
	               uint32_t *notchanged, uint32_t *erased, uint32_t *repaired) override;
	uint8_t repairAsync(const FsContext &context, inode_t inode, uint8_t correctOnly,
	                    std::shared_ptr<FileRepairStats> stats,
	                    const std::function<void(int)> &callback) override;

	/// Removes an empty directory from the filesystem.
	/// @see IFilesystemOperations::rmdir
	uint8_t rmdir(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	              inode_t parent, const HString &name) override;

	/// Removes a filesystem entry and all nested contents as a task-manager job.
	/// @see IFilesystemOperations::recursiveRemove
	uint8_t recursiveRemove(const FsContext &context, inode_t parent, const HString &name,
	                        const std::function<void(int)> &callback, uint32_t job_id) override;

	/// Calculates the serialized directory listing size for readdirData().
	/// @see IFilesystemOperations::readdirSize
	uint8_t readdirSize(const FsContext &context, inode_t inode, uint8_t flags, void **dnode,
	                    uint32_t *dbuffsize) override;

	/// Serializes directory entries into a buffer sized by readdirSize().
	/// @see IFilesystemOperations::readdirData
	void readdirData(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                 uint8_t flags, void *dnode, uint8_t *dbuff) override;

	/// Reads a paginated list of entries from a directory.
	/// @see IFilesystemOperations::readdir
	uint8_t readdir(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                inode_t inode, uint64_t first_entry, uint64_t number_of_entries,
	                std::vector<DirectoryEntry> &dir_entries) override;

	uint8_t checkFile(const FsContext &context, inode_t inode,
	                  ChunkCountArray &chunkCount) override;
	uint8_t checkFileAsync(const FsContext &context, inode_t inode,
	                       std::shared_ptr<ChunkCountArray> chunkCount,
	                       const std::function<void(int)> &callback) override;
	uint8_t openCheck(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                  inode_t inode, uint8_t flags, Attributes &attr) override;
	uint8_t getGoal(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                inode_t inode, uint8_t gmode, GoalStatistics &fgtab,
	                GoalStatistics &dgtab) override;

	/// Retrieves aggregated extra-attribute statistics for a node or subtree.
	/// @see IFilesystemOperations::getExtraAttr
	uint8_t getExtraAttr(const FsContext &context, inode_t inode, uint8_t gmode,
	                     ExtraAttributesArray &fileEAttrTab,
	                     ExtraAttributesArray &dirEAttrTab) override;

	/// Lists extended attribute names for an inode.
	/// @see IFilesystemOperations::listXAttr
	uint8_t listXAttr(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                  inode_t inode, uint8_t opened, XAttrListResult &result,
	                  uint32_t *xasize) override;

	/// Retrieves the value of a single extended attribute.
	/// @see IFilesystemOperations::getXAttr
	uint8_t getXAttr(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                 inode_t inode, uint8_t opened, uint8_t anleng, const uint8_t *attrname,
	                 XAttrGetResult &result) override;

	/// Sets, replaces, or removes an extended attribute on an inode.
	/// @see IFilesystemOperations::setXAttr
	uint8_t setXAttr(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                 inode_t inode, uint8_t opened, uint8_t anleng, const uint8_t *attrname,
	                 uint32_t avleng, const uint8_t *attrvalue, uint8_t mode) override;

	/// Removes (unlinks) a file or non-directory node from the filesystem.
	/// @see IFilesystemOperations::unlink
	uint8_t unlink(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	               inode_t parent, const HString &name) override;

	/// Returns chunk ids, versions, and locations for a file chunk range.
	/// @see IFilesystemOperations::getChunksInfo
	uint8_t getChunksInfo(const FsContext &context, uint32_t current_ip, inode_t inode,
	                      uint32_t chunk_index, uint32_t chunk_count,
	                      std::vector<ChunkWithAddressAndLabel> &chunks) override;

	/// Aggregates trash-time counters for a node or subtree.
	/// @see IFilesystemOperations::getTrashTimePrepare
	uint8_t getTrashTimePrepare(const FsContext &context, inode_t inode, uint8_t gmode,
	                            TrashtimeMap &fileTrashtimes, TrashtimeMap &dirTrashtimes) override;

	/// Merges a POSIX ACL (kAccess or kDefault) into the node's stored RichACL.
	/// @see IFilesystemOperations::setAcl
	uint8_t setAcl(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	               inode_t inode, AclType type, const AccessControlList &acl) override;

	/// Stores a RichACL directly on a node, replacing any previously stored ACL.
	/// @see IFilesystemOperations::setAcl
	uint8_t setAcl(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	               inode_t inode, const RichACL &acl) override;

	/// Retrieves the stored RichACL for a node, given its inode.
	/// @see IFilesystemOperations::getAcl
	uint8_t getAcl(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	               inode_t inode, RichACL &acl) override;

	// Functions which modify metadata or return some information.
	// To be used by the master server with personality == kMaster

	/// Retrieves filesystem-wide capacity and object counters.
	/// @see IFilesystemOperations::getFSStats
	void getFSStats(uint64_t *totalSpace, uint64_t *availableSpace, uint64_t *trashSpace,
	                inode_t *trashNodes, uint64_t *reservedSpace, inode_t *reservedNodes,
	                inode_t *inodes, inode_t *directoryNodes, inode_t *fileNodes,
	                inode_t *linkNodes) override;

	/// Returns the byte length of the full path string for a directory node.
	/// @see IFilesystemOperations::getDirPathSize
	uint32_t getDirPathSize(const FilesystemOperationContext &fsOpContext, inode_t inode) override;

	/// Writes the full path string for a directory node into a caller-supplied buffer.
	/// @see IFilesystemOperations::getDirPathData
	void getDirPathData(const FilesystemOperationContext &fsOpContext, inode_t inode, uint8_t *buff,
	                    uint32_t size) override;

	/// Resolves a filesystem path to a directory inode, starting from the root.
	/// @see IFilesystemOperations::getRootInode
	uint8_t getRootInode(inode_t *rootinode, const uint8_t *path) override;

	uint8_t readChunk(const FilesystemOperationContext &fsOpContext, inode_t inode, uint32_t indx,
	                  uint64_t *chunkid, uint64_t *length) override;
	uint8_t writeEnd(const FilesystemOperationContext &fsOpContext, inode_t inode, uint64_t length,
	                 uint64_t chunkid, uint32_t lockid, uint64_t grantGeneration,
	                 uint64_t grantRandom) override;
	void getTrashTimeStore(TrashtimeMap &fileTrashtimes, TrashtimeMap &dirTrashtimes,
	                       uint8_t *buff) override;

	uint32_t newSessionId() override;

	// RESERVED
	uint8_t readReservedSize(inode_t rootinode, [[maybe_unused]] uint8_t sesflags,
	                         uint32_t *dbuffsize) override;
	void readReservedData([[maybe_unused]] inode_t rootinode, [[maybe_unused]] uint8_t sesflags,
	                      uint8_t *dbuff) override;
	void readReserved(uint32_t off, uint32_t max_entries,
	                  std::vector<NamedInodeEntry> &entries) override;
	void readReserved(const FilesystemOperationContext &fsOpContext, uint64_t handleOffset,
	                  uint32_t maxEntries, std::vector<HandleInodeEntry> &entries) override;

	// TRASH
	uint8_t readTrashSize(inode_t rootinode, [[maybe_unused]] uint8_t sesflags,
	                      uint32_t *dbuffsize) override;
	void readTrashData([[maybe_unused]] inode_t rootinode, [[maybe_unused]] uint8_t sesflags,
	                   uint8_t *dbuff) override;
	void readTrash(uint32_t off, uint32_t max_entries,
	               std::vector<NamedInodeEntry> &entries) override;
	void readTrash(const FilesystemOperationContext &fsOpContext, uint64_t handleOffset,
	               uint32_t maxEntries, std::vector<HandleInodeEntry> &entries) override;
	uint8_t getTrashPath(const FilesystemOperationContext &fsOpContext, inode_t rootinode,
	                     [[maybe_unused]] uint8_t sesflags, inode_t inode,
	                     std::string &path) override;

	// RESERVED+TRASH
	uint8_t getDetachedAttr(const FilesystemOperationContext &fsOpContext, inode_t rootinode,
	                        [[maybe_unused]] uint8_t sesflags, inode_t inode, Attributes &attr,
	                        uint8_t dtype) override;

	// EXTRA
	uint8_t getDirStats(const FsContext &context, inode_t inode, inode_t *inodes, inode_t *dirs,
	                    inode_t *files, inode_t *links, uint32_t *chunks, uint64_t *length,
	                    uint64_t *size, uint64_t *rsize) override;
	uint8_t getChunkId(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                   inode_t inode, uint32_t index, uint64_t *chunkid) override;

	// SPECIAL - LOG EMERGENCY INCREASE VERSION FROM CHUNKS-MODULE
	void increaseChunkVersion(const FilesystemOperationContext &fsOpContext,
	                          uint64_t chunkid) override;

	uint8_t fullPathByInode(const FsContext &context, inode_t inode,
	                        std::string &fullPath) override;
	std::string fullPathByInode(const FilesystemOperationContext &fsOpContext,
	                            inode_t initialInode) override;

	/// Purges expired trash entries whose expiry timestamp is before @p timeStamp.
	/// @see IFilesystemOperations::doEmptyTrash
	void doEmptyTrash(uint32_t timeStamp) override;

	/// Forcefully releases every reserved file from each of its owning sessions, regardless
	/// of whether those sessions are still active.
	/// @see IFilesystemOperations::doEmptyReserved
	void doEmptyReserved(uint32_t timeStamp) override;

	/// No-op: the in-memory backend completes file operations synchronously.
	/// @see IFilesystemOperations::drainPendingFileOperations
	void drainPendingFileOperations([[maybe_unused]] uint32_t timeStamp) override {}
#endif

	// QUOTAS

	/// Checks hard user/group quota limits for resource deltas.
	/// @see IFilesystemOperations::quotaExceededUg
	QuotaCheckResult quotaExceededUg(
	    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, uint32_t uid, uint32_t gid,
	    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) override;

	/// Checks hard directory-owner quota limits for a node context.
	/// @see IFilesystemOperations::quotaExceededDir
	QuotaCheckResult quotaExceededDir(
	    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node,
	    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) override;

	/// Checks destination-side hard directory quota limits for move/rename.
	/// @see IFilesystemOperations::quotaExceededDirMove
	QuotaCheckResult quotaExceededDirMove(
	    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNodeDirectory *node,
	    FSNodeDirectory *prevNode,
	    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) override;

	/// Performs combined user/group and directory hard quota checks.
	/// @see IFilesystemOperations::quotaExceeded
	QuotaCheckResult quotaExceeded(
	    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node,
	    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) override;

	/// Runs user/group then directory hard quota checks, propagating read-failure status.
	/// @see IFilesystemOperations::checkQuotaUgDir
	uint8_t checkQuotaUgDir(
	    const FilesystemOperationContext &fsOpContext, uint32_t uid, uint32_t gid, FSNode *dir,
	    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) override;

	/// Applies quota usage deltas for successful node mutations.
	/// @see IFilesystemOperations::quotaUpdate
	void quotaUpdate(
	    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node,
	    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) override;

	/// Removes all quota tuples for one owner.
	/// @see IFilesystemOperations::quotaRemove
	void quotaRemove([[maybe_unused]] const FilesystemOperationContext &fsOpContext,
	                 QuotaOwnerType ownerType, inode_t ownerId) override;

#ifndef METARESTORE
	/// Returns all quota entries visible to the caller.
	/// @see IFilesystemOperations::quotaGetAll
	uint8_t quotaGetAll(const FsContext &context, std::vector<QuotaEntry> &results) override;

	/// Returns quota entries for selected owners.
	/// @see IFilesystemOperations::quotaGet
	uint8_t quotaGet(const FsContext &context, const std::vector<QuotaOwner> &owners,
	                 std::vector<QuotaEntry> &results) override;

	/// Sets quota tuples and emits SETQUOTA changelog entries.
	/// @see IFilesystemOperations::quotaSet
	uint8_t quotaSet(const FsContext &context,
	                 [[maybe_unused]] const FilesystemOperationContext &fsOpContext,
	                 const std::vector<QuotaEntry> &entries) override;

	/// Builds display information for quota entries.
	/// @see IFilesystemOperations::quotaGetInfo
	uint8_t quotaGetInfo(const FsContext &context, const std::vector<QuotaEntry> &entries,
	                     std::vector<std::string> &result) override;

	// CHECKSUM

	/// Starts recalculating metadata checksum in background.
	/// @see IFilesystemOperations::fs_start_checksum_recalculation.
	uint8_t startChecksumRecalculation() override;

	/// Runs the once-per-second file-test scheduler tick.
	/// @see IFilesystemOperations::fsTestPeriodicTick
	void fsTestPeriodicTick(uint32_t timeStamp) override;

	/// Runs one background file-test step from the event loop.
	/// @see IFilesystemOperations::fsTestBackgroundStep
	void fsTestBackgroundStep() override;

	/// Returns the last published file-test report.
	/// @see IFilesystemOperations::fsTestGetData
	void fsTestGetData(FsTestReport &out) override;

	/// Returns a paginated list of defective files.
	/// @see IFilesystemOperations::fsTestGetDefectiveNodes
	std::vector<DefectiveFileInfo> fsTestGetDefectiveNodes(uint8_t requestedFlags,
	                                                       uint64_t maxEntries,
	                                                       uint64_t &cursor) override;

	/// Removes file-test state for a deleted node.
	/// @see IFilesystemOperations::fsTestOnNodeRemoved
	void fsTestOnNodeRemoved(const FilesystemOperationContext &fsOpContext, inode_t inode) override;

	/// Runs one background metadata-checksum recalculation step.
	/// @see IFilesystemOperations::backgroundChecksumStep
	void backgroundChecksumStep() override;

	/// In-memory backend has no backend-specific periodic options.
	/// @see IFilesystemOperations::readBackendPeriodicConfig
	void readBackendPeriodicConfig() override {}
#endif
	void addFilesToChunks(bool isMetadataLoading = true) override;

	// Functions which apply changes from changelog, only for shadow master and metarestore
	uint8_t applyChecksum(const std::string &version, uint64_t checksum) override;
	uint8_t applyCreate(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                    inode_t parent, const HString &name, FSNodeType type, uint32_t mode,
	                    uint32_t uid, uint32_t gid, uint32_t rdev, inode_t inode) override;
	uint8_t applyAccess(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                    inode_t inode) override;
	uint8_t applyAttr(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                  inode_t inode, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t atime,
	                  uint32_t mtime) override;
	uint8_t applySession(uint32_t sessionid) override;
	uint8_t applyIncreaseChunkVersion(const FilesystemOperationContext &fsOpContext,
	                                  uint64_t chunkid) override;
	uint8_t applyLength(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                    inode_t inode, uint64_t length, bool eraseFurtherChunks) override;
	uint8_t applyRepair(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                    inode_t inode, uint32_t indx, uint32_t nversion) override;
	uint8_t applySetXAttr(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                      inode_t inode, uint32_t anleng, const uint8_t *attrname, uint32_t avleng,
	                      const uint8_t *attrvalue, uint32_t mode) override;

	/// Replays a SETACL changelog entry during metadata restore.
	/// @see IFilesystemOperations::applySetAcl
	uint8_t applySetAcl(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                    inode_t inode, char aclType, const char *aclString) override;

	/// Replays a SETRICHACL changelog entry during metadata restore.
	/// @see IFilesystemOperations::applySetRichAcl
	uint8_t applySetRichAcl(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                        inode_t inode, const std::string &acl_string) override;

	uint8_t applyUnlink(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                    inode_t parent, const HString &name, inode_t inode) override;
	uint8_t applyUnlock(uint64_t chunkid) override;
	uint8_t applyTrunc(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                   inode_t inode, uint32_t indx, uint64_t chunkid, uint32_t lockid) override;

	/// Replays a SETQUOTA changelog tuple update.
	/// @see IFilesystemOperations::applySetQuota
	uint8_t applySetQuota([[maybe_unused]] const FilesystemOperationContext &fsOpContext,
	                      char rigor, char resource, char ownerType, inode_t ownerId,
	                      uint64_t limit) override;

	// CHECKSUM

	/// Returns checksum of the loaded metadata.
	uint64_t metadataChecksum(ChecksumMode mode) override;

	/// In-memory backend supports Master-Shadow metadata checksums.
	/// @see IFilesystemOperations::metadataChecksumSupported
	bool metadataChecksumSupported() const override { return true; }

	// Locks

	/// Perform a flock operation on filesystem.
	/// @see IFilesystemOperations::flockOperation.
	int flockOperation(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                   inode_t inode, uint64_t owner, uint32_t sessionid, uint32_t reqid,
	                   uint32_t msgid, uint16_t oper, bool nonblocking,
	                   std::vector<FileLocks::Owner> &applied) override;

	/// Perform a posix lock operation on filesystem.
	/// @see IFilesystemOperations::posixLockOperation.
	int posixLockOperation(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                       inode_t inode, uint64_t start, uint64_t end, uint64_t owner,
	                       uint32_t sessionid, uint32_t reqid, uint32_t msgid, uint16_t oper,
	                       bool nonblocking, std::vector<FileLocks::Owner> &applied) override;

	/// Perform a POSIX lock probe on filesystem.
	/// @see IFilesystemOperations::posixLockProbe.
	int posixLockProbe(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                   inode_t inode, uint64_t start, uint64_t end, uint64_t owner,
	                   uint32_t sessionid, uint32_t reqid, uint32_t msgid, uint16_t oper,
	                   safs_locks::FlockWrapper &info) override;

	/// Release (unlock + unqueue) all locks from a given session.
	/// @see IFilesystemOperations::locksClearSession.
	int locksClearSession(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                      uint8_t type, inode_t inode, uint32_t sessionid,
	                      std::vector<FileLocks::Owner> &applied) override;

	/// List locks in the filesystem.
	/// @see IFilesystemOperations::locksListAll.
	int locksListAll(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                 uint8_t type, bool pending, uint64_t start, uint64_t max,
	                 std::vector<safs_locks::Info> &outLocks) override;

	/// List locks for a specific inode.
	/// @see IFilesystemOperations::locksListInode.
	int locksListInode(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                   uint8_t type, bool pending, inode_t inode, uint64_t start, uint64_t max,
	                   std::vector<safs_locks::Info> &outLocks) override;

	/// Unlocks the matching locks on the specified inode and tries to apply pending locks.
	/// @see IFilesystemOperations::locksUnlockInode.
	int locksUnlockInode(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                     uint8_t type, inode_t inode,
	                     std::vector<FileLocks::Owner> &applied) override;

	/// Removes a pending lock matching the provided parameters.
	/// @see IFilesystemOperations::locksRemovePending.
	int locksRemovePending(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                       uint8_t type, uint64_t ownerid, uint32_t sessionid, inode_t inode,
	                       uint64_t reqid) override;

protected:
	/// Backend-specific hook for retrieving an xattr value.
	/// Default implementation reads from in-memory hash tables.
	/// @param fsOpContext Operation context (provides transaction for KV backends).
	/// @param inode The inode to retrieve the attribute from.
	/// @param anleng Length of the attribute name.
	/// @param attrname The attribute name bytes.
	/// @param[out] result Populated with an owned copy of the attribute value.
	virtual uint8_t doConcreteGetXAttr(
	    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, inode_t inode,
	    uint8_t anleng, const uint8_t *attrname, XAttrGetResult &result);

	/// Backend-specific hook for listing xattr names for an inode.
	/// Default implementation reads from in-memory hash tables.
	/// @param fsOpContext Operation context (provides transaction for KV backends).
	/// @param inode The inode to list attributes for.
	/// @param[out] result Populated with serialized xattr name data.
	/// @param[in,out] xasize Pre-initialized with sizeof(kAclXattrs); add name sizes here.
	virtual uint8_t doConcreteListXAttr(
	    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, inode_t inode,
	    XAttrListResult &result, uint32_t *xasize);

	/// Backend-specific hook for setting/removing an xattr.
	/// Default implementation modifies in-memory hash tables.
	/// @param fsOpContext Operation context (provides transaction for KV backends).
	/// @param inode The inode to set/remove the attribute on.
	/// @param anleng Length of the attribute name.
	/// @param attrname The attribute name bytes.
	/// @param avleng Length of the attribute value.
	/// @param attrvalue The attribute value bytes.
	/// @param mode One of XATTR_SMODE_* (create, replace, remove, etc.).
	virtual uint8_t doConcreteSetXAttr(
	    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, inode_t inode,
	    uint8_t anleng, const uint8_t *attrname, uint32_t avleng, const uint8_t *attrvalue,
	    uint32_t mode);

	/// Backend-specific hook for retrieving a detached trash/reserved path.
	/// Default implementation reads from in-memory trash/reserved containers.
	/// @param fsOpContext Operation context (provides transaction for KV backends).
	/// @param node Detached trash/reserved node whose stored path should be retrieved.
	/// @return Stored detached path without leading slash or suffix, or std::nullopt if absent.
	virtual std::optional<std::string> getDetachedPath(
	    [[maybe_unused]] const FilesystemOperationContext &fsOpContext,
	    const FSNode *node) override;

	/// Permission check for lock operations.
	/// Validates that the caller has appropriate read/write access for the
	/// requested lock type. Shared by posixLockProbe and lockOperation.
	static int checkLockPermissions(const FsContext &context,
	                                const FilesystemOperationContext &fsOpContext, inode_t inode,
	                                uint16_t op);

	/// Core lock operation logic shared by flockOperation and posixLockOperation.
	/// Operates on the given FileLocks reference, enabling KV overrides to pass
	/// a temporary FileLocks loaded from FDB instead of gMetadata.
	int lockOperation(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                  FileLocks &locks, inode_t inode, uint64_t start, uint64_t end, uint64_t owner,
	                  uint32_t sessionid, uint32_t reqid, uint32_t msgid, uint16_t oper,
	                  bool nonblocking, std::vector<FileLocks::Owner> &applied);

	/// Try applying pending locks after an unlock.
	/// Operates on the given FileLocks reference for the same reason as lockOperation.
	void manageLockTryLockPending(FileLocks &locks, inode_t inode, uint64_t start, uint64_t end,
	                              std::vector<FileLocks::Owner> &applied);

private:
	/// Node operations object
	std::unique_ptr<IFilesystemNodeOperations> nodeOperations_;
};

/// Registers the check consulted by fs_commit_pipeline_idle. The client-server
/// layer registers its group-commit pipeline state at init; nothing registered
/// means no pipeline exists and the state reads idle.
void fs_register_commit_pipeline_idle_check(bool (*check)());

/// True when no client group-commit batch is open, held, or in flight.
/// Background maintenance whose own transactions must not commit between a
/// batch's staged writes and that batch's commit consults this before writing.
bool fs_commit_pipeline_idle();
