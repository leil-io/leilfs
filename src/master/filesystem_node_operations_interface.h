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

#include <array>
#include <string>
#include <vector>

#include "common/attributes.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_trash_reserved_files.h"
#include "master/fs_context.h"
#include "protocol/directory_entry.h"
#include "protocol/handle_inode_entry.h"
#include "protocol/named_inode_entry.h"

class HString;
class RichACL;
class AccessControlList;
class FsContext;
struct StatsRecord;
struct DirectoryEntry;
struct HandleInodeEntry;
struct NamedInodeEntry;

using ChunkCountArray = std::array<uint32_t, CHUNK_MATRIX_SIZE>;

/// Interface for filesystem node operations extensibility.
///
/// Classes implementing this interface can be used to override default filesystem node behavior.
class IFilesystemNodeOperations {
public:
	/// Default constructor
	IFilesystemNodeOperations() = default;

	/// Virtual destructor
	virtual ~IFilesystemNodeOperations() = default;

	/// Non-copyable and non-movable
	IFilesystemNodeOperations(const IFilesystemNodeOperations &) = delete;
	IFilesystemNodeOperations &operator=(const IFilesystemNodeOperations &) = delete;
	IFilesystemNodeOperations(IFilesystemNodeOperations &&) = delete;
	IFilesystemNodeOperations &operator=(IFilesystemNodeOperations &&) = delete;

	// Type-safe node lookup operations

	template <class NodeType>
	NodeType *idToNodeVerify(inode_t inode) {
		auto *node = static_cast<NodeType *>(this->idToNodeInternal(inode));
		this->checkNodeType(node);
		return node;
	}

	template <class NodeType = FSNode>
	NodeType *idToNode(inode_t inode) {
		return static_cast<NodeType *>(this->idToNodeInternal(inode));
	}

	// Main node operations

	virtual FSNode *lookup(FSNodeDirectory *node, const HString &name) const = 0;

	virtual FSNode *createNode(uint32_t timeStamp, FSNodeDirectory *parent, const HString &name,
	                           FSNodeType type, uint16_t mode, uint16_t umask, uint32_t uid,
	                           uint32_t gid, uint8_t copysgid, AclInheritance inheritAcl,
	                           inode_t requestedINode = 0) = 0;
	virtual void link(uint32_t timeStamp, FSNodeDirectory *parent, FSNode *child,
	                  const HString &name) = 0;
	virtual void unlink(uint32_t timeStamp, FSNodeDirectory *parent, const HString &childName,
	                    FSNode *childNode) = 0;
	virtual void removeEdge(uint32_t timeStamp, FSNodeDirectory *parent, const HString &childName,
	                        FSNode *childNode) = 0;

	virtual void updateCTime(FSNode *node, uint32_t ctime) = 0;
	virtual void fillAttr(FSNode *node, FSNode *parent, uint32_t uid, uint32_t gid, uint32_t auid,
	                      uint32_t agid, uint8_t sesflags, Attributes &attr) = 0;
	virtual void fillAttr(const FsContext &context, FSNode *node, FSNode *parent,
	                      Attributes &attr) = 0;
	virtual void getStats(FSNode *node, StatsRecord *statsOut) = 0;
	virtual void addStats(FSNodeDirectory *parent, StatsRecord *stats) = 0;
	virtual void addSubStats(FSNodeDirectory *parent, StatsRecord *newStats,
	                         StatsRecord *previousStats) = 0;
	virtual void changeUidGid(FSNode *node, uint32_t uid, uint32_t gid) = 0;

	virtual void setLength(FSNodeFile *obj, uint64_t length, bool eraseFurtherChunks) = 0;
	virtual uint8_t appendChunks(uint32_t timeStamp, FSNodeFile *destNodeFile,
	                             FSNodeFile *srcNodeFile) = 0;
	virtual void changeFileGoal(FSNodeFile *nodeFile, uint8_t goal) = 0;
#ifndef METARESTORE
	virtual void checkFile(FSNodeFile *nodeFile, ChunkCountArray &chunkCount) = 0;
#endif
	virtual int64_t getSize(FSNode *node) = 0;

#ifndef METARESTORE
	virtual uint32_t getDirSize(const FSNodeDirectory *nodeDir, uint8_t withAttr) = 0;
	virtual void getDirData(inode_t rootINode, uint32_t uid, uint32_t gid, uint32_t auid,
	                        uint32_t agid, uint8_t sesflags, FSNodeDirectory *nodeDir,
	                        uint8_t *outBuffer, uint8_t withAttr) = 0;

	/// Get entries of directory node \a nodeDir.
	///
	/// Returns directory entries in \a dirEntriesOut container.
	/// \a firstEntry == 0 means the very first entry in the directory.
	/// \param nodeDir directory node to get the entries of
	/// \param firstEntry index of the first dirent to get
	/// \param numberOfEntries number of dirents to get
	/// \param[out] dirEntriesOut container into which dirents are inserted
	virtual void getDir(inode_t rootINode, uint32_t uid, uint32_t gid, uint32_t auid, uint32_t agid,
	                    uint8_t sesflags, FSNodeDirectory *nodeDir, uint64_t firstEntry,
	                    uint64_t numberOfEntries, std::vector<DirectoryEntry> &dirEntriesOut) = 0;
#endif
	virtual int isNameUsed(FSNodeDirectory *node, const HString &name) = 0;

	// Trash/Reserved operations

	virtual int purge(uint32_t timeStamp, FSNode *node) = 0;
	virtual uint8_t undel(uint32_t timeStamp, FSNodeFile *node) = 0;
#ifndef METARESTORE
	virtual uint32_t getDetachedSize(const TrashPathContainer &data) = 0;
	virtual void getDetachedData(const TrashPathContainer &data, uint8_t *outBuffer) = 0;
	virtual void getDetachedData(const TrashPathContainer &data, uint32_t offset,
	                             uint32_t maxEntries, std::vector<NamedInodeEntry> &entries) = 0;
	virtual uint32_t getDetachedSize(const ReservedPathContainer &data) = 0;
	virtual void getDetachedData(const ReservedPathContainer &data, uint8_t *outBuffer) = 0;
	virtual void getDetachedData(const ReservedPathContainer &data, uint32_t offset,
	                             uint32_t maxEntries, std::vector<NamedInodeEntry> &entries) = 0;

	/// Returns entries from a HandleIndexContainer starting at a given handleOffset.
	/// The offset provided by the client (e.g., FUSE readdir) is always non-negative (sign bit 0).
	/// Internally, the server may store offsets with the sign bit set (bit 63 = 1).
	/// All lookups are enforced to be done ignoring the sign bit by setting it to 0.
	virtual void getDetachedData(const HandleIndexContainer &data, uint64_t handleOffset,
	                             uint32_t maxEntries, std::vector<HandleInodeEntry> &entries,
	                             bool fromTrash) = 0;
#endif

	// Path operations
	virtual void getPath(FSNodeDirectory *parent, FSNode *child, std::string &path) = 0;
	virtual uint32_t getPathSize(FSNodeDirectory *parent, FSNode *child) = 0;
	virtual void getPathData(FSNodeDirectory *parent, FSNode *child, uint8_t *path,
	                         uint32_t size) = 0;
	virtual std::string escapeName(const std::string &name) = 0;

	// ACL operations
	virtual uint8_t setAcl(FSNode *node, const RichACL &acl, uint32_t timeStamp) = 0;
	virtual uint8_t setAcl(FSNode *node, AclType type, const AccessControlList &acl,
	                       uint32_t timeStamp) = 0;
#ifndef METARESTORE
	virtual uint8_t getAcl(FSNode *node, RichACL &acl) = 0;
#endif  // METARESTORE
	virtual uint8_t deleteAcl(FSNode *node, AclType type, uint32_t timeStamp) = 0;

	// Recursive operations
#ifndef METARESTORE
	virtual void getGoalRecursive(FSNode *node, uint8_t gmode, GoalStatistics &fgtab,
	                              GoalStatistics &dgtab) = 0;
	virtual void getTrashTimeRecursive(FSNode *node, uint8_t gmode, TrashtimeMap &fileTrashtimes,
	                                   TrashtimeMap &dirTrashtimes) = 0;
	virtual void getEAttrRecursive(FSNode *node, uint8_t gmode, uint32_t feattrtab[16],
	                               uint32_t deattrtab[16]) = 0;
#endif
	virtual void setgoalRecursive(FSNode *node, uint32_t timeStamp, uint32_t uid, uint8_t goal,
	                              uint8_t smode, inode_t *modifiedINodesOut,
	                              inode_t *unchangedINodesOut,
	                              inode_t *permissionDeniedINodesOut) = 0;

	virtual void setTrashTimeRecursive(FSNode *node, uint32_t timeStamp, uint32_t uid,
	                                   uint32_t trashtime, uint8_t smode,
	                                   inode_t *modifiedINodesOut, inode_t *unchangedINodesOut,
	                                   inode_t *permissionDeniedINodesOut) = 0;

	virtual void setEAttrRecursive(FSNode *node, uint32_t timeStamp, uint32_t uid, uint8_t eattr,
	                               uint8_t smode, inode_t *modifiedINodesOut,
	                               inode_t *unchangedINodesOut,
	                               inode_t *permissionDeniedINodesOut) = 0;

	// Access control operations
	virtual int access(const FsContext &context, FSNode *node, uint8_t modemask) = 0;
	virtual int stickyAccess(FSNode *parent, FSNode *node, uint32_t uid) = 0;
	virtual int nameCheck(const std::string &name) = 0;
	virtual uint8_t verifySession(const FsContext &context, OperationMode operationMode,
	                              SessionType sessionType) = 0;

	/// Treating rootinode as the root of the hierarchy, converts (rootinode, inode) to FSNode*.
	/// ie:
	/// if inode == rootinode, then returns root node
	/// if inode != rootinode, then returns some node
	/// Checks for permissions needed to perform the operation (defined by modemask).
	/// Can return a reserved node or a node from trash.
	virtual uint8_t getNodeForOperation(const FsContext &context, ExpectedNodeType expectedNodeType,
	                                    uint8_t modemask, inode_t inode, FSNode **nodeOut,
	                                    FSNodeDirectory **rootDirOut = nullptr) = 0;

	// Ancestry operations

	/// Returns true if \a ancestor is ancestor of \a node.
	/// @param ancestor potential ancestor node
	/// @param node potential descendant node
	virtual bool isAncestor(FSNodeDirectory *ancestor, FSNode *node) = 0;

	/// Returns true if \a node is reserved or in trash or \a ancestor is ancestor of \a node.
	/// @param ancestor potential ancestor node
	/// @param node potential reserved, trash or descendant node
	virtual bool isAncestorOrNodeReservedOrTrash(FSNodeDirectory *ancestor, FSNode *node) = 0;
	virtual FSNodeDirectory *getFirstParent(FSNode *node) = 0;

protected:
	// Core node lookup operation - override in subclasses for custom storage
	virtual FSNode *idToNodeInternal(inode_t inode) = 0;

	// Type checking helper - base case
	template <class NodeType>
	void checkNodeType(const NodeType *node) {
		assert(node);
		(void)node;
	}
};

// Template specializations for type checking
template <>
inline void IFilesystemNodeOperations::checkNodeType(const FSNodeFile *node) {
	assert(node && (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
	                node->type == FSNodeType::kReserved));
	(void)node;
}

template <>
inline void IFilesystemNodeOperations::checkNodeType(const FSNodeDirectory *node) {
	assert(node && node->type == FSNodeType::kDirectory);
	(void)node;
}

template <>
inline void IFilesystemNodeOperations::checkNodeType(const FSNodeSymlink *node) {
	assert(node && node->type == FSNodeType::kSymlink);
	(void)node;
}

template <>
inline void IFilesystemNodeOperations::checkNodeType(const FSNodeDevice *node) {
	assert(node && (node->type == FSNodeType::kBlockDev || node->type == FSNodeType::kCharDev));
	(void)node;
};
