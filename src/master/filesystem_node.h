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

#include "master/filesystem_node_operations_interface.h"
#include "master/filesystem_node_types.h"
#include "master/fs_context.h"
#include "protocol/directory_entry.h"
#include "protocol/handle_inode_entry.h"
#include "protocol/named_inode_entry.h"

/// Base class for filesystem node operations extensibility.
///
/// Provides default implementations for all methods of IFilesystemNodeOperations interface,
/// assuming an in-memory metadata representation.
class FilesystemNodeOperationsBase : public IFilesystemNodeOperations {
public:
	FSNode *lookup(FSNodeDirectory *node, const HString &name) const override;

	FSNode *createNode(uint32_t timeStamp, FSNodeDirectory *parent, const HString &name,
	                   FSNodeType type, uint16_t mode, uint16_t umask, uint32_t uid, uint32_t gid,
	                   uint8_t copysgid, AclInheritance inheritAcl,
	                   inode_t requestedINode = 0) override;
	void link(uint32_t timeStamp, FSNodeDirectory *parent, FSNode *child,
	          const HString &name) override;
	void unlink(uint32_t timeStamp, FSNodeDirectory *parent, const HString &childName,
	            FSNode *childNode) override;
	void removeEdge(uint32_t timeStamp, FSNodeDirectory *parent, const HString &childName,
	                FSNode *childNode) override;

	void updateCTime(FSNode *node, uint32_t ctime) override;
	void fillAttr(FSNode *node, FSNode *parent, uint32_t uid, uint32_t gid, uint32_t auid,
	              uint32_t agid, uint8_t sesflags, Attributes &attr) override;
	void fillAttr(const FsContext &context, FSNode *node, FSNode *parent,
	              Attributes &attr) override;
	void getStats(FSNode *node, StatsRecord *statsOut) override;
	void addStats(FSNodeDirectory *parent, StatsRecord *stats) override;
	void addSubStats(FSNodeDirectory *parent, StatsRecord *newStats,
	                 StatsRecord *previousStats) override;
	void changeUidGid(FSNode *node, uint32_t uid, uint32_t gid) override;

	void setLength(FSNodeFile *obj, uint64_t length, bool eraseFurtherChunks) override;
	uint8_t appendChunks(uint32_t timeStamp, FSNodeFile *destNodeFile,
	                     FSNodeFile *srcNodeFile) override;
	void changeFileGoal(FSNodeFile *nodeFile, uint8_t goal) override;
#ifndef METARESTORE
	void checkFile(FSNodeFile *nodeFile, uint32_t chunkCount[CHUNK_MATRIX_SIZE]) override;
#endif
	int64_t getSize(FSNode *node) override;

#ifndef METARESTORE
	uint32_t getDirSize(const FSNodeDirectory *nodeDir, uint8_t withAttr) override;
	void getDirData(inode_t rootINode, uint32_t uid, uint32_t gid, uint32_t auid, uint32_t agid,
	                uint8_t sesflags, FSNodeDirectory *nodeDir, uint8_t *outBuffer,
	                uint8_t withAttr) override;

	/// Get entries of directory node \a nodeDir.
	/// @see IFilesystemNodeOperations::getDir
	void getDir(inode_t rootINode, uint32_t uid, uint32_t gid, uint32_t auid, uint32_t agid,
	            uint8_t sesflags, FSNodeDirectory *nodeDir, uint64_t firstEntry,
	            uint64_t numberOfEntries, std::vector<DirectoryEntry> &dirEntriesOut) override;
#endif
	int isNameUsed(FSNodeDirectory *node, const HString &name) override;

	// Trash/Reserved operations
	int purge(uint32_t timeStamp, FSNode *node) override;
	uint8_t undel(uint32_t timeStamp, FSNodeFile *node) override;
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
	void getDetachedData(const HandleIndexContainer &data, uint64_t handleOffset,
	                     uint32_t maxEntries, std::vector<HandleInodeEntry> &entries,
	                     bool fromTrash) override;
#endif

	// Path operations
	void getPath(FSNodeDirectory *parent, FSNode *child, std::string &path) override;
	uint32_t getPathSize(FSNodeDirectory *parent, FSNode *child) override;
	void getPathData(FSNodeDirectory *parent, FSNode *child, uint8_t *path, uint32_t size) override;
	std::string escapeName(const std::string &name) override;

	// ACL operations
	uint8_t setAcl(FSNode *node, const RichACL &acl, uint32_t timeStamp) override;
	uint8_t setAcl(FSNode *node, AclType type, const AccessControlList &acl,
	               uint32_t timeStamp) override;
#ifndef METARESTORE
	uint8_t getAcl(FSNode *node, RichACL &acl) override;
#endif  // METARESTORE
	uint8_t deleteAcl(FSNode *node, AclType type, uint32_t timeStamp) override;

	// Recursive operations
#ifndef METARESTORE
	void getGoalRecursive(FSNode *node, uint8_t gmode, GoalStatistics &fgtab,
	                      GoalStatistics &dgtab) override;
	void getTrashTimeRecursive(FSNode *node, uint8_t gmode, TrashtimeMap &fileTrashtimes,
	                           TrashtimeMap &dirTrashtimes) override;
	void getEAttrRecursive(FSNode *node, uint8_t gmode, uint32_t feattrtab[16],
	                       uint32_t deattrtab[16]) override;
#endif  // METARESTORE
	void setgoalRecursive(FSNode *node, uint32_t timeStamp, uint32_t uid, uint8_t goal,
	                      uint8_t smode, inode_t *modifiedINodesOut, inode_t *unchangedINodesOut,
	                      inode_t *permissionDeniedINodesOut) override;

	void setTrashTimeRecursive(FSNode *node, uint32_t timeStamp, uint32_t uid, uint32_t trashtime,
	                           uint8_t smode, inode_t *modifiedINodesOut,
	                           inode_t *unchangedINodesOut,
	                           inode_t *permissionDeniedINodesOut) override;

	void setEAttrRecursive(FSNode *node, uint32_t timeStamp, uint32_t uid, uint8_t eattr,
	                       uint8_t smode, inode_t *modifiedINodesOut, inode_t *unchangedINodesOut,
	                       inode_t *permissionDeniedINodesOut) override;

	// Access control operations
	int access(const FsContext &context, FSNode *node, uint8_t modemask) override;
	int stickyAccess(FSNode *parent, FSNode *node, uint32_t uid) override;
	int nameCheck(const std::string &name) override;
	uint8_t verifySession(const FsContext &context, OperationMode operationMode,
	                      SessionType sessionType) override;

	/// Treating rootinode as the root of the hierarchy, converts (rootinode, inode) to FSNode*.
	/// @see IFilesystemNodeOperations::getNodeForOperation
	uint8_t getNodeForOperation(const FsContext &context, ExpectedNodeType expectedNodeType,
	                            uint8_t modemask, inode_t inode, FSNode **nodeOut,
	                            FSNodeDirectory **rootDirOut = nullptr) override;

	// Ancestry operations

	/// Returns true if \a ancestor is ancestor of \a node.
	/// @see IFilesystemNodeOperations::isAncestor
	bool isAncestor(FSNodeDirectory *ancestor, FSNode *node) override;

	/// Returns true if \a node is reserved or in trash or \a ancestor is ancestor of \a node.
	/// @see IFilesystemNodeOperations::isAncestorOrNodeReservedOrTrash
	bool isAncestorOrNodeReservedOrTrash(FSNodeDirectory *ancestor, FSNode *node) override;

	FSNodeDirectory *getFirstParent(FSNode *node) override;

protected:
	/// Internal node lookup operation - override in subclasses for custom storage
	FSNode *idToNodeInternal(inode_t inode) override;

private:
	// Private helpers
	void fsnodes_sub_stats(FSNodeDirectory *parent, StatsRecord *stats);
	void fsnodes_remove_node(uint32_t timeStamp, FSNode *node);

	uint32_t last_chunk_blocks(FSNodeFile *node);
	bool last_chunk_nonempty(FSNodeFile *node);
	uint32_t file_chunks(FSNodeFile *node);
	uint64_t file_size(FSNodeFile *node, uint32_t nonzero_chunks);
	uint64_t file_realsize(FSNodeFile *node, uint32_t nonzero_chunks, uint64_t file_size);
#ifndef METARESTORE
	uint32_t ec_chunk_realsize(uint32_t blocks, uint32_t data_part_count,
	                           uint32_t parity_part_count);
#endif
};
