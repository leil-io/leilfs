/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
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

#include <initializer_list>
#include <optional>

#include "master/filesystem_node_operations_interface.h"
#include "master/filesystem_node_types.h"
#include "master/fs_context.h"
#include "protocol/directory_entry.h"
#include "protocol/handle_inode_entry.h"
#include "protocol/named_inode_entry.h"
#include "protocol/quota.h"

/// Base class for filesystem node operations extensibility.
///
/// Provides default implementations for all methods of IFilesystemNodeOperations interface,
/// assuming an in-memory metadata representation.
class FilesystemNodeOperationsBase : public IFilesystemNodeOperations {
public:
	/// Returns the root node of the filesystem.
	FSNodeDirectory *getRootNode(const FilesystemOperationContext &fsOpContext) override;

	/// Looks up a child node by name within a directory.
	/// @see IFilesystemNodeOperations::lookup
	FSNode *lookup([[maybe_unused]] const FilesystemOperationContext &fsOpContext,
	               FSNodeDirectory *node, const HString &name,
	               bool isCaseInsensitive = false) const override;

	FSNode *createNode(const FilesystemOperationContext &fsOpContext, uint32_t timeStamp,
	                   FSNodeDirectory *parent, const HString &name, FSNodeType type, uint16_t mode,
	                   uint16_t umask, uint32_t uid, uint32_t gid, uint8_t copysgid,
	                   AclInheritance inheritAcl, inode_t requestedINode = 0) override;

	/// Syncs the current state of the node to persistent storage on backends that need it.
	/// @see IFilesystemNodeOperations::updateNode
	/// @note This implementation is a no-op for in-memory storage.
	void updateNode([[maybe_unused]] const FilesystemOperationContext &fsOpContext,
	                [[maybe_unused]] FSNode *node) override;

	void link(const FilesystemOperationContext &fsOpContext, uint32_t timeStamp,
	          FSNodeDirectory *parent, FSNode *child, const HString &name) override;

	/// Unlink the child node from the parent directory.
	/// @see IFilesystemNodeOperations::unlink
	void unlink(const FilesystemOperationContext &fsOpContext, uint32_t timeStamp,
	            FSNodeDirectory *parent, const HString &childName, FSNode *childNode) override;

	/// Remove the edge between parent and child nodes.
	/// @see IFilesystemNodeOperations::removeEdge
	void removeEdge(const FilesystemOperationContext &fsOpContext, uint32_t timeStamp,
	                FSNodeDirectory *parent, const HString &childName, FSNode *childNode) override;

	void updateCTime([[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node,
	                 uint32_t ctime) override;

	/// Updates ctime on a trash node whose trashtime has just changed.
	/// @see IFilesystemNodeOperations::updateCTimeForTrashNode
	void updateCTimeForTrashNode([[maybe_unused]] const FilesystemOperationContext &fsOpContext,
	                             FSNode *node, uint32_t newCtime, uint32_t oldTrashtime) override;

	void fillAttr(const FilesystemOperationContext &fsOpContext, FSNode *node, FSNode *parent,
	              uint32_t uid, uint32_t gid, uint32_t auid, uint32_t agid, uint8_t sesflags,
	              Attributes &attr) override;
	void fillAttr(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	              FSNode *node, FSNode *parent, Attributes &attr) override;

	/// Retrieves statistics for a filesystem node.
	/// @see IFilesystemNodeOperations::getStats
	void getStats([[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node,
	              StatsRecord *statsOut) override;

	/// Adds statistics to a directory and recursively propagates to all ancestors.
	/// @see IFilesystemNodeOperations::addStats
	void addStats(const FilesystemOperationContext &fsOpContext, FSNodeDirectory *parent,
	              StatsRecord *stats) override;

	/// Subtracts statistics from a directory and recursively propagates to all ancestors.
	/// @see IFilesystemNodeOperations::subStats
	void subStats(const FilesystemOperationContext &fsOpContext, FSNodeDirectory *parent,
	              StatsRecord *stats) override;

	/// Updates directory statistics by propagating the delta between old and new stats.
	/// @see IFilesystemNodeOperations::addSubStats
	void addSubStats(const FilesystemOperationContext &fsOpContext, FSNodeDirectory *parent,
	                 StatsRecord *newStats, StatsRecord *previousStats) override;

	/// Updates all parent directories' statistics based on a node's stats change.
	/// @see IFilesystemNodeOperations::updateParentStatsForNode
	void updateParentStatsForNode(const FilesystemOperationContext &fsOpContext, FSNode *node,
	                              StatsRecord *newStats, StatsRecord *previousStats) override;

	void changeUidGid(const FilesystemOperationContext &fsOpContext, FSNode *node, uint32_t uid,
	                  uint32_t gid) override;

	void setLength(const FilesystemOperationContext &fsOpContext, FSNodeFile *nodeFile,
	               uint64_t length, bool eraseFurtherChunks) override;
	uint8_t appendChunks(const FilesystemOperationContext &fsOpContext, uint32_t timeStamp,
	                     FSNodeFile *destNodeFile, FSNodeFile *srcNodeFile) override;
	void changeFileGoal(const FilesystemOperationContext &fsOpContext, FSNodeFile *nodeFile,
	                    uint8_t goal) override;
#ifndef METARESTORE
	void checkFile(FSNodeFile *nodeFile, ChunkCountArray &chunkCount) override;
#endif
	int64_t getSize(const FilesystemOperationContext &fsOpContext, FSNode *node) override;

	/// Returns the number of parents of the given node.
	/// @see IFilesystemNodeOperations::getNumberOfParents
	/// @note fsOpContext is unused in this in-memory implementation.
	uint64_t getNumberOfParents(const FilesystemOperationContext &fsOpContext,
	                            const FSNode *node) override;

	/// No-op: the in-memory master stores atime in the node (the caller already bumped it).
	/// @see IFilesystemNodeOperations::persistNodeAtime
	void persistNodeAtime(const FilesystemOperationContext &fsOpContext, FSNode *node,
	                      uint32_t timeStamp) override;

	/// No-op: the in-memory master keeps a node's atime in the node itself.
	/// @see IFilesystemNodeOperations::resetNodeAtime
	void resetNodeAtime(const FilesystemOperationContext &fsOpContext, FSNode *node) override;

	/// No-op: the in-memory master keeps a directory's mtime in the node itself.
	/// @see IFilesystemNodeOperations::resetDirChildChangeTime
	void resetDirChildChangeTime(const FilesystemOperationContext &fsOpContext,
	                             FSNodeDirectory *dir, uint32_t opTimeStamp) override;

protected:
	/// Conflict-free persistence of a directory's child-change time: records
	/// that an entry of `dir` was added or removed at `timeStamp` WITHOUT rewriting (and
	/// conflicting on) the whole parent node. The default is a no-op: backends that persist
	/// the directory node itself (the in-memory master) keep mtime/ctime in the node. The
	/// KV backend overrides this with an FDB atomicMax on the DIR_CHILD_CHANGE_TIME_ key.
	virtual void persistDirChildChangeTime(const FilesystemOperationContext &fsOpContext,
	                                       FSNodeDirectory *dir, uint32_t timeStamp);

	/// Returns the directory's persisted child-change time, or 0 if none. Used to reconcile
	/// a directory's served mtime/ctime (max of the node field and this value) when a child
	/// create/remove did not rewrite the node. Default 0: the node's own timestamps are
	/// authoritative (in-memory master).
	virtual uint32_t getDirChildChangeTime(const FilesystemOperationContext &fsOpContext,
	                                       const FSNodeDirectory *dir);

	/// Conflict-free persistence of a directory's direct-subdirectory count delta:
	/// records +1/-1 on every subdirectory link/unlink WITHOUT rewriting the parent
	/// node, co-located with the in-memory FSNodeDirectory::nlink update. Default no-op: the
	/// in-memory master keeps nlink in the node. The KV backend overrides with an FDB
	/// atomicAdd on the DIR_SUBDIRS_ key.
	virtual void persistDirSubdirCountDelta(const FilesystemOperationContext &fsOpContext,
	                                        FSNodeDirectory *dir, int64_t delta);

	/// Returns the directory's served link count. Default: the node's own nlink field (the
	/// in-memory master). The KV backend overrides this to 2 + the persisted direct-subdir
	/// count, so nlink survives a reload without rewriting the node per child create.
	virtual uint32_t getDirNlink(const FilesystemOperationContext &fsOpContext,
	                             const FSNodeDirectory *dir);

	/// Returns a node's persisted access time, or 0 if none. Used to reconcile the served
	/// atime (max of the node field and this value) when a read advanced atime without
	/// rewriting the node. Default 0: the node's own atime is authoritative (in-memory master).
	/// The KV backend overrides this with a read of the NODE_ATIME_ key.
	virtual uint32_t getNodeAtime(const FilesystemOperationContext &fsOpContext,
	                              const FSNode *node);

public:
#ifndef METARESTORE
	uint32_t getDirSize(const FSNodeDirectory *nodeDir, uint8_t withAttr) override;
	void getDirData(const FilesystemOperationContext &fsOpContext, inode_t rootINode, uint32_t uid,
	                uint32_t gid, uint32_t auid, uint32_t agid, uint8_t sesflags,
	                FSNodeDirectory *nodeDir, uint8_t *outBuffer, uint8_t withAttr) override;

	/// Returns the number of entries in the given directory.
	/// @see IFilesystemNodeOperations::getNumberOfDirEntries
	/// @note fsOpContext is unused in this in-memory implementation.
	uint64_t getNumberOfDirEntries(const FilesystemOperationContext &fsOpContext,
	                               const FSNodeDirectory *nodeDir) override;

	/// Get entries of directory node \a nodeDir.
	/// @see IFilesystemNodeOperations::getDir
	void getDir(const FilesystemOperationContext &fsOpContext, inode_t rootINode, uint32_t uid,
	            uint32_t gid, uint32_t auid, uint32_t agid, uint8_t sesflags,
	            FSNodeDirectory *nodeDir, uint64_t firstEntry, uint64_t numberOfEntries,
	            std::vector<DirectoryEntry> &dirEntriesOut) override;
#endif

	/// Returns direct child inode IDs for a directory.
	/// @see IFilesystemNodeOperations::getDirectoryChildInodes
	std::vector<inode_t> getDirectoryChildInodes(const FilesystemOperationContext &fsOpContext,
	                                             const FSNodeDirectory *nodeDir) override;

	/// Returns direct child (name, inode) edges for a directory.
	/// @see IFilesystemNodeOperations::getDirectoryChildEdges
	std::vector<std::pair<HString, inode_t>> getDirectoryChildEdges(
	    const FilesystemOperationContext &fsOpContext, const FSNodeDirectory *nodeDir) override;

	/// Resolves the stored-case child name via the directory's in-memory lowercase index.
	/// @see IFilesystemNodeOperations::getBaseStoredChildName
	std::string getBaseStoredChildName(const FilesystemOperationContext &fsOpContext,
	                                   FSNodeDirectory *nodeDir,
	                                   const HString &anyCaseName) override;

	/// Checks if a name is already used in the given directory.
	/// @see IFilesystemNodeOperations::isNameUsed
	bool isNameUsed(const FilesystemOperationContext &fsOpContext, FSNodeDirectory *node,
	                const HString &name, bool isCaseInsensitive = false) override;

	// Trash/Reserved operations
	int purge(const FilesystemOperationContext &fsOpContext, uint32_t timeStamp,
	          FSNode *node) override;
	uint8_t undel(const FilesystemOperationContext &fsOpContext, uint32_t timeStamp,
	              FSNodeFile *node) override;
#ifndef METARESTORE
	uint32_t getDetachedSize(const TrashPathContainer &data) override;
	void getDetachedData(const TrashPathContainer &data, uint8_t *outBuffer) override;
	void getDetachedData(const TrashPathContainer &data, uint32_t offset, uint32_t maxEntries,
	                     std::vector<NamedInodeEntry> &entries) override;
	uint32_t getDetachedSize(const ReservedPathContainer &data) override;
	void getDetachedData(const ReservedPathContainer &data, uint8_t *outBuffer) override;
	void getDetachedData(const ReservedPathContainer &data, uint32_t offset, uint32_t maxEntries,
	                     std::vector<NamedInodeEntry> &entries) override;

	/// Returns entries from a HandleIndexContainer starting at a given handleOffset.
	/// @see IFilesystemNodeOperations::getDetachedData
	void getDetachedData(const FilesystemOperationContext &fsOpContext,
	                     const HandleIndexContainer &data, uint64_t handleOffset,
	                     uint32_t maxEntries, std::vector<HandleInodeEntry> &entries,
	                     bool fromTrash) override;
#endif

	// Path operations
	void getPath(const FilesystemOperationContext &fsOpContext, FSNodeDirectory *parent,
	             FSNode *child, std::string &path) override;
	uint32_t getPathSize(const FilesystemOperationContext &fsOpContext, FSNodeDirectory *parent,
	                     FSNode *child) override;
	void getPathData(const FilesystemOperationContext &fsOpContext, FSNodeDirectory *parent,
	                 FSNode *child, uint8_t *path, uint32_t size) override;
	std::string escapeName(const std::string &name) override;

	// ACL operations

	/// Stores a RichACL on a node, replacing any previously stored ACL.
	/// @see IFilesystemNodeOperations::setAcl
	uint8_t setAcl(const FilesystemOperationContext &fsOpContext, FSNode *node, const RichACL &acl,
	               uint32_t timeStamp) override;

	/// Merges a POSIX ACL into the node's stored RichACL.
	/// @see IFilesystemNodeOperations::setAcl
	uint8_t setAcl(const FilesystemOperationContext &fsOpContext, FSNode *node, AclType type,
	               const AccessControlList &acl, uint32_t timeStamp) override;

#ifndef METARESTORE
	/// Retrieves the stored RichACL for a node.
	/// @see IFilesystemNodeOperations::getAcl
	uint8_t getAcl(const FilesystemOperationContext &fsOpContext, FSNode *node,
	               RichACL &acl) override;
#endif  // METARESTORE

	/// Removes or prunes the ACL stored on a node according to the ACL type.
	/// @see IFilesystemNodeOperations::deleteAcl
	uint8_t deleteAcl(const FilesystemOperationContext &fsOpContext, FSNode *node, AclType type,
	                  uint32_t timeStamp) override;

	/// Re-aligns a node's stored ACL with the standard permission bits in node->mode.
	/// @see IFilesystemNodeOperations::syncAclWithMode
	void syncAclWithMode(const FilesystemOperationContext &fsOpContext, FSNode *node) override;

	// Recursive operations
#ifndef METARESTORE
	void getGoalRecursive(const FilesystemOperationContext &fsOpContext, FSNode *node,
	                      uint8_t gmode, GoalStatistics &fileGoalsTab,
	                      GoalStatistics &dirGoalsTab) override;
	void getTrashTimeRecursive(FSNode *node, uint8_t gmode, TrashtimeMap &fileTrashtimes,
	                           TrashtimeMap &dirTrashtimes) override;

	/// Aggregates extra-attribute histogram counters for a node or subtree.
	/// @see IFilesystemNodeOperations::getExtraAttrRecursive
	void getExtraAttrRecursive(FSNode *node, uint8_t gmode, ExtraAttributesArray &fileEAttrTab,
	                           ExtraAttributesArray &dirEAttrTab) override;
#endif  // METARESTORE
	void setgoalRecursive(const FilesystemOperationContext &fsOpContext, FSNode *node,
	                      uint32_t timeStamp, uint32_t uid, uint8_t goal, uint8_t smode,
	                      inode_t *modifiedINodesOut, inode_t *unchangedINodesOut,
	                      inode_t *permissionDeniedINodesOut) override;

	void setTrashTimeRecursive(FSNode *node, uint32_t timeStamp, uint32_t uid, uint32_t trashtime,
	                           uint8_t smode, inode_t *modifiedINodesOut,
	                           inode_t *unchangedINodesOut,
	                           inode_t *permissionDeniedINodesOut) override;

	/// Applies extra-attribute updates on a node or subtree and tracks outcomes.
	/// @see IFilesystemNodeOperations::setExtraAttrRecursive
	void setExtraAttrRecursive(const FilesystemOperationContext &fsOpContext, FSNode *node,
	                           uint32_t timeStamp, uint32_t uid, uint8_t eattr, uint8_t smode,
	                           inode_t *modifiedINodesOut, inode_t *unchangedINodesOut,
	                           inode_t *permissionDeniedINodesOut) override;

	// Access control operations
	int access(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	           FSNode *node, uint8_t modeMask) override;

protected:
	/// Returns the stored RichACL for @p node, or nullptr if none is present.
	/// @p scratch is an optional caller-supplied buffer; implementations that need
	/// to materialise a temporary ACL (e.g. KV backends) emplace into @p scratch
	/// and return &scratch->value(), while the default in-memory implementation
	/// leaves @p scratch empty and returns a pointer into aclStorage directly.
	/// The caller avoids constructing a RichACL when it is not needed.
	virtual const RichACL *getAclForAccess(const FilesystemOperationContext &fsOpContext,
	                                       FSNode *node, std::optional<RichACL> &scratch);

	/// Persists the ACL a freshly created @p node inherits from its parent's default ACL.
	/// The default in-memory implementation stores into aclStorage; backends that keep ACLs
	/// elsewhere (e.g. KV) override to persist there. @p acl is consumed.
	virtual void storeInheritedAcl(const FilesystemOperationContext &fsOpContext, FSNode *node,
	                               RichACL &&acl);

	int stickyAccess(FSNode *parent, FSNode *node, uint32_t uid) override;
	int nameCheck(const std::string &name) override;
	uint8_t verifySession(const FsContext &context, OperationMode operationMode,
	                      SessionType sessionType) override;

	/// Treating rootinode as the root of the hierarchy, converts (rootinode, inode) to FSNode*.
	/// @see IFilesystemNodeOperations::getNodeForOperation
	uint8_t getNodeForOperation(const FsContext &context,
	                            const FilesystemOperationContext &fsOpContext,
	                            ExpectedNodeType expectedNodeType, uint8_t modeMask, inode_t inode,
	                            FSNode **nodeOut, FSNodeDirectory **rootDirOut = nullptr) override;

	// Ancestry operations

	/// Returns true if \a ancestor is ancestor of \a node.
	/// @see IFilesystemNodeOperations::isAncestor
	bool isAncestor(const FilesystemOperationContext &fsOpContext, FSNodeDirectory *ancestor,
	                FSNode *node) override;

	/// Returns true if \a node is reserved or in trash or \a ancestor is ancestor of \a node.
	/// @see IFilesystemNodeOperations::isAncestorOrNodeReservedOrTrash
	bool isAncestorOrNodeReservedOrTrash(const FilesystemOperationContext &fsOpContext,
	                                     FSNodeDirectory *ancestor, FSNode *node) override;

	FSNodeDirectory *getFirstParent(const FilesystemOperationContext &fsOpContext,
	                                FSNode *node) override;

	/// @see IFilesystemNodeOperations::getFirstParentId
	inode_t getFirstParentId([[maybe_unused]] const FilesystemOperationContext &fsOpContext,
	                         FSNode *node) override;

	/// Returns the IDs of all parents of the given node.
	/// @see IFilesystemNodeOperations::getParentIds
	std::vector<inode_t> getParentIds(
	    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node) override;

	/// Returns the edge name that links parentId -> node.
	/// @see IFilesystemNodeOperations::getChildNameByParentId
	std::string getChildNameByParentId(const FilesystemOperationContext &fsOpContext,
	                                   inode_t parentId, const FSNode *node) override;

	/// Internal node lookup operation with context - override in subclasses for custom storage.
	/// @see IFilesystemNodeOperations::idToNodeInternal
	FSNode *idToNodeInternal(const FilesystemOperationContext &fsOpContext,
	                         inode_t inode) const override;

	/// Increases the node counters for the specified type.
	/// @see IFilesystemNodeOperations::incrementNodeCounters
	void incrementNodeCounters(const FilesystemOperationContext &fsOpContext,
	                           FSNodeType type) override;

	/// Preserves the given node in the underlying storage (in-memory in this implementation).
	/// @see IFilesystemNodeOperations::preserveNode
	void preserveNode(const FilesystemOperationContext &fsOpContext, FSNode *node) override;

	/// Preserves the edge between parent and child in the underlying storage (in-memory in this
	/// implementation).
	/// @see IFilesystemNodeOperations::preserveEdge
	void preserveEdge(const FilesystemOperationContext &fsOpContext, FSNodeDirectory *parent,
	                  FSNode *child, hstorage::Handle *handlePtr) override;

	/// Updates owner quota usage for node mutations.
	/// In-memory backend applies updates directly to quota structures.
	/// KV backend can override to persist counters in the active transaction.
	virtual void nodeQuotaUpdate(
	    const FilesystemOperationContext &fsOpContext, FSNode *node,
	    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList);

	/// Removes all quota tuples for a specific owner.
	/// In-memory backend removes from quota database.
	/// KV backend can override to remove owner keys from persistent storage.
	virtual void nodeQuotaRemove(const FilesystemOperationContext &fsOpContext,
	                             QuotaOwnerType ownerType, inode_t ownerId);

	/// Updates detached trash/reserved space counters for a file size mutation.
	/// In-memory backend updates `gMetadata->trashSpace` / `gMetadata->reservedSpace`.
	/// KV backends can override to persist the delta in the active transaction.
	virtual void updateDetachedSpaceUsage(const FilesystemOperationContext &fsOpContext,
	                                      const FSNodeFile *nodeFile, uint64_t previousLength,
	                                      uint64_t newLength);

	/// Validates a trash/reserved path string for use with undel().
	///
	/// Checks that the path is non-empty, contains no NUL bytes, has no "//" sequences,
	/// no "." or ".." components, and that all name segments fit within kMaxFileNameLength.
	///
	/// @param pathStr  The full path string as stored in trash/reserved (leading '/' is stripped).
	/// @return SAUNAFS_STATUS_OK if the path is valid; SAUNAFS_ERROR_CANTCREATEPATH otherwise.
	static uint8_t validateTrashPath(const std::string &pathStr);

	/// Frees a node and performs all associated in-memory and KV cleanup.
	///
	/// Caller is responsible for removing the NODE_ key from KV and decrementing the
	/// kMetaNodesKey / kMetaFileNodesKey counters (via atomicAdd) BEFORE calling this.
	/// This method deletes chunks, releases the inode, cleans up xattrs, quota, and
	/// node hash, then destroys the node object.
	void removeNode(const FilesystemOperationContext &fsOpContext, uint32_t timeStamp,
	                FSNode *node);

private:
	/// Number of blocks in the last chunk before EOF
	static uint32_t lastChunkBlocks(FSNodeFile *node);

	/// Does the last chunk exist and contain non-zero data?
	static bool isLastChunkNonEmpty(FSNodeFile *node);

	/// Count chunks in a file, disregard sparse file holes
	static uint32_t fileChunksCount(FSNodeFile *node);

	/// Compute the "size" statistic for a file node
	static uint64_t fileSize(FSNodeFile *node, uint32_t nonZeroChunks);

	/// Compute the "realsize" statistic for a file node.
	/// @param node file node (used e.g. to detect a partial last chunk and goal).
	/// @param nonZeroChunks number of non-empty chunks (used for EC/XOR slice calculations).
	/// @param logicalFileSize logical file "size" as returned by fileSize(...).
	static uint64_t fileRealSize(FSNodeFile *node, uint32_t nonZeroChunks,
	                             uint64_t logicalFileSize);

#ifndef METARESTORE
	/// Compute the disk space cost of all parts of a xor/ec chunk of given size
	static uint32_t ecChunkRealSize(uint32_t blocks, uint32_t dataPartCount,
	                                uint32_t parityPartCount);
#endif
};
