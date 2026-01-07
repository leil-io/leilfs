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
#include <cstdint>
#include <string>
#include <vector>

#include "common/attributes.h"
#include "common/type_defs.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_operation_context.h"
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

/// Maximum length for a file or directory name in bytes
inline constexpr size_t kMaxFileNameLength = 255;

/// All permission bits (including setuid/setgid/sticky)
inline constexpr uint16_t kPermissionsMask = 07777;
/// Standard rwx permissions (excludes setuid/setgid/sticky)
inline constexpr uint16_t kStandardPermissionsMask = 0777;
inline constexpr uint16_t kExtraAttributesMask = 0xF000;  ///< Bits 12-15 for extra attributes

/// Default undelete directory mode
inline constexpr uint16_t kUndelDirectoryMode = 0755;

// Extra attributes constants and type aliases
inline constexpr uint8_t kMaxExtraAttributes = 16;
using ExtraAttributesArray = std::array<uint32_t, kMaxExtraAttributes>;

// Directory entry serialization constants
inline constexpr size_t kDirEntryWithAttributesSize = kinode_t_size + kAttributesSize;
inline constexpr size_t kDirEntryWithoutAttributesSize = kinode_t_size + 1;  // +1 for node type
inline constexpr size_t kDotEntrySize = 1;                                   // "." (1 byte)
inline constexpr size_t kDotDotEntrySize = 2;                                // ".." (2 bytes)

// For readdir related operations
inline constexpr uint64_t kSignBit64 = (1ULL << 63ULL);
// special entryIndex values
inline constexpr uint64_t kDotEntryIndex = 0;
inline constexpr uint64_t kDotDotEntryIndex =
    (static_cast<uint64_t>(1) << hstorage::Handle::kHashShift);
inline constexpr uint64_t kUnusedEntryIndex =
    (static_cast<uint64_t>(2) << hstorage::Handle::kHashShift);

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

	/// Looks up a node by its inode and verifies its type.
	template <class NodeType>
	NodeType *idToNodeVerify(inode_t inode) {
		auto *node = static_cast<NodeType *>(this->idToNodeInternal(inode));
		this->checkNodeType(node);
		return node;
	}

	/// Looks up a node by its inode and context, and verifies its type.
	template <class NodeType>
	NodeType *idToNodeVerify(const FilesystemOperationContext &fsOpContext, inode_t inode) {
		auto *node = static_cast<NodeType *>(this->idToNodeInternal(fsOpContext, inode));
		this->checkNodeType(node);
		return node;
	}

	/// Looks up a node by its inode.
	template <class NodeType = FSNode>
	NodeType *idToNode(inode_t inode) {
		return static_cast<NodeType *>(this->idToNodeInternal(inode));
	}

	/// Looks up a node by its inode and context.
	template <class NodeType = FSNode>
	NodeType *idToNode(const FilesystemOperationContext &fsOpContext, inode_t inode) {
		return static_cast<NodeType *>(this->idToNodeInternal(fsOpContext, inode));
	}

	/// Returns the root node of the filesystem.
	virtual FSNodeDirectory *getRootNode(const FilesystemOperationContext &fsOpContext) = 0;

	// Main node operations

	virtual FSNode *lookup(FSNodeDirectory *node, const HString &name) const = 0;

	virtual FSNode *createNode(const FilesystemOperationContext &fsOpContext, uint32_t timeStamp,
	                           FSNodeDirectory *parent, const HString &name, FSNodeType type,
	                           uint16_t mode, uint16_t umask, uint32_t uid, uint32_t gid,
	                           uint8_t copysgid, AclInheritance inheritAcl,
	                           inode_t requestedINode = 0) = 0;
	virtual void link(const FilesystemOperationContext &fsOpContext, uint32_t timeStamp,
	                  FSNodeDirectory *parent, FSNode *child, const HString &name) = 0;

	/// Removes the last link to a node from the filesystem.
	///
	/// This method handles the removal of the edge between parent and child, and manages
	/// the final disposal of the node if this was its last parent link. Depending on the
	/// node's configuration and state, the node may be:
	/// - Moved to trash (if trashtime > 0)
	/// - Moved to reserved (if still has open sessions)
	/// - Permanently deleted (if neither condition applies)
	///
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param timeStamp The current timestamp for the operation.
	/// @param parent The parent directory containing the child.
	/// @param childName The name of the child entry in the parent directory.
	/// @param childNode The child node to unlink.
	///
	/// @note Directories can only have one parent, but files may have multiple hard links.
	///       The node is only removed/trashed when the last link is removed.
	/// @note This method calls removeEdge() to handle the edge removal and parent updates.
	virtual void unlink(const FilesystemOperationContext &fsOpContext, uint32_t timeStamp,
	                    FSNodeDirectory *parent, const HString &childName, FSNode *childNode) = 0;

	/// Removes an edge (directory entry) between a parent directory and a child node.
	///
	/// This method removes the directory entry from the parent, updates the child's parent list,
	/// adjusts parent directory statistics (size, timestamps, nlink), and handles case-insensitive
	/// filesystems. Unlike unlink(), this method only removes the link itself and does NOT handle
	/// the final disposal of nodes (moving to trash/reserved or deletion).
	///
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param timeStamp The current timestamp for updating mtime/ctime.
	/// @param parent The parent directory containing the edge.
	/// @param childName The name of the child entry to remove.
	/// @param childNode The child node being unlinked from the parent.
	///
	/// @note This is called by unlink() as part of the unlinking process.
	/// @note After removing the edge, the child may still exist if it has other parent links.
	/// @note Parent directory's nlink is decremented only if the child is a directory.
	/// @note Emits an edgeRemovedSignal after the edge is removed.
	virtual void removeEdge(const FilesystemOperationContext &fsOpContext, uint32_t timeStamp,
	                        FSNodeDirectory *parent, const HString &childName,
	                        FSNode *childNode) = 0;

	virtual void updateCTime(FSNode *node, uint32_t ctime) = 0;
	virtual void fillAttr(FSNode *node, FSNode *parent, uint32_t uid, uint32_t gid, uint32_t auid,
	                      uint32_t agid, uint8_t sesflags, Attributes &attr) = 0;
	virtual void fillAttr(const FsContext &context, FSNode *node, FSNode *parent,
	                      Attributes &attr) = 0;
	virtual void getStats(FSNode *node, StatsRecord *statsOut) = 0;
	virtual void addStats(FSNodeDirectory *parent, StatsRecord *stats) = 0;
	virtual void subStats(FSNodeDirectory *parent, StatsRecord *stats) = 0;
	virtual void addSubStats(FSNodeDirectory *parent, StatsRecord *newStats,
	                         StatsRecord *previousStats) = 0;
	virtual void changeUidGid(FSNode *node, uint32_t uid, uint32_t gid) = 0;

	virtual void setLength(FSNodeFile *nodeFile, uint64_t length, bool eraseFurtherChunks) = 0;
	virtual uint8_t appendChunks(uint32_t timeStamp, FSNodeFile *destNodeFile,
	                             FSNodeFile *srcNodeFile) = 0;
	virtual void changeFileGoal(FSNodeFile *nodeFile, uint8_t goal) = 0;
#ifndef METARESTORE
	virtual void checkFile(FSNodeFile *nodeFile, ChunkCountArray &chunkCount) = 0;
#endif
	virtual int64_t getSize(FSNode *node) = 0;

	/// Returns the number of parents of the given node, possibly reusing the transaction
	/// inside fsOpContext.
	/// @param fsOpContext The filesystem operation context potentially containing a transaction.
	/// @param node The node whose parents are to be counted.
	/// @return The number of parents of the node.
	/// @note Directories can't have multiple parents to keep a tree structure, but files can have
	///       multiple hard links.
	/// @note In-memory backend: could return node->parents.size() directly.
	/// @note KV backend: should use kDirParentKeyPrefix or kParentKeyPrefix according to node type.
	virtual uint64_t getNumberOfParents(const FilesystemOperationContext &fsOpContext,
	                                    const FSNode *node) = 0;

#ifndef METARESTORE
	virtual uint32_t getDirSize(const FSNodeDirectory *nodeDir, uint8_t withAttr) = 0;
	virtual void getDirData(inode_t rootINode, uint32_t uid, uint32_t gid, uint32_t auid,
	                        uint32_t agid, uint8_t sesflags, FSNodeDirectory *nodeDir,
	                        uint8_t *outBuffer, uint8_t withAttr) = 0;

	/// Returns the number of entries in the given directory, possibly reusing the transaction
	/// inside fsOpContext.
	/// @param fsOpContext The filesystem operation context potentially containing a transaction.
	/// @param nodeDir The directory node whose entries are to be counted.
	/// @return The number of entries in the directory.
	/// @note In-memory backend: could return entries.size() directly.
	/// @note KV backend: gets atomic counter at kDirNodesCountPrefix_<inode> for O(1) access.
	virtual uint64_t getNumberOfDirEntries(const FilesystemOperationContext &fsOpContext,
	                                       const FSNodeDirectory *nodeDir) = 0;

	/// @brief Get entries of directory node using pagination.
	///
	/// Returns directory entries in the output container, handling special entries
	/// "." and ".." automatically when starting from index 0. The function uses
	/// Handle-based indexing where the sign bit (bit 63) is stripped from indices
	/// returned to clients.
	///
	/// @param fsOpContext Filesystem operation context (used by some implementations).
	/// @param rootINode The root inode of the session's visible hierarchy. Used to determine if the
	///                  directory is the root (affects "." and ".." inode values).
	/// @param uid User ID for attribute filling.
	/// @param gid Group ID for attribute filling.
	/// @param auid Access User ID for attribute filling.
	/// @param agid Access Group ID for attribute filling.
	/// @param sesflags Session flags for attribute filling.
	/// @param nodeDir Directory node to get the entries of.
	/// @param firstEntry Index of the first directory entry to retrieve. Use 0 for
	///                   the "." entry. Subsequent calls should use the `next_index`
	///                   value from the last returned entry for continuation.
	/// @param numberOfEntries Maximum number of directory entries to retrieve (page size).
	/// @param[out] dirEntriesOut Container into which directory entries are inserted.
	///
	/// @note The special index values are:
	///       - 0: represents the "." entry
	///       - (1 << kHashShift): represents the ".." entry
	///       - (2 << kHashShift): represents an unused/end marker
	/// @note The `next_index` in the last entry will be the unused marker if there
	///       are no more entries in the directory.
	virtual void getDir(const FilesystemOperationContext &fsOpContext, inode_t rootINode,
	                    uint32_t uid, uint32_t gid, uint32_t auid, uint32_t agid, uint8_t sesflags,
	                    FSNodeDirectory *nodeDir, uint64_t firstEntry, uint64_t numberOfEntries,
	                    std::vector<DirectoryEntry> &dirEntriesOut) = 0;
#endif
	virtual bool isNameUsed(FSNodeDirectory *node, const HString &name) = 0;

	// Trash/Reserved operations

	virtual int purge(uint32_t timeStamp, FSNode *node) = 0;
	virtual uint8_t undel(const FilesystemOperationContext &fsOpContext, uint32_t timeStamp,
	                      FSNodeFile *node) = 0;
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
	virtual void getGoalRecursive(FSNode *node, uint8_t gmode, GoalStatistics &fileGoalsTab,
	                              GoalStatistics &dirGoalsTab) = 0;
	virtual void getTrashTimeRecursive(FSNode *node, uint8_t gmode, TrashtimeMap &fileTrashtimes,
	                                   TrashtimeMap &dirTrashtimes) = 0;
	virtual void getExtraAttrRecursive(FSNode *node, uint8_t gmode,
	                                   ExtraAttributesArray &fileEAttrTab,
	                                   ExtraAttributesArray &dirEAttrTab) = 0;
#endif
	virtual void setgoalRecursive(FSNode *node, uint32_t timeStamp, uint32_t uid, uint8_t goal,
	                              uint8_t smode, inode_t *modifiedINodesOut,
	                              inode_t *unchangedINodesOut,
	                              inode_t *permissionDeniedINodesOut) = 0;

	virtual void setTrashTimeRecursive(FSNode *node, uint32_t timeStamp, uint32_t uid,
	                                   uint32_t trashtime, uint8_t smode,
	                                   inode_t *modifiedINodesOut, inode_t *unchangedINodesOut,
	                                   inode_t *permissionDeniedINodesOut) = 0;

	virtual void setExtraAttrRecursive(FSNode *node, uint32_t timeStamp, uint32_t uid,
	                                   uint8_t eattr, uint8_t smode, inode_t *modifiedINodesOut,
	                                   inode_t *unchangedINodesOut,
	                                   inode_t *permissionDeniedINodesOut) = 0;

	// Access control operations
	virtual int access(const FsContext &context, FSNode *node, uint8_t modeMask) = 0;
	virtual int stickyAccess(FSNode *parent, FSNode *node, uint32_t uid) = 0;
	virtual int nameCheck(const std::string &name) = 0;
	virtual uint8_t verifySession(const FsContext &context, OperationMode operationMode,
	                              SessionType sessionType) = 0;

	/// Treating rootinode as the root of the hierarchy, converts (rootinode, inode) to FSNode*.
	/// ie:
	/// if inode == rootinode, then returns root node
	/// if inode != rootinode, then returns some node
	/// Checks for permissions needed to perform the operation (defined by modeMask).
	/// Can return a reserved node or a node from trash.
	virtual uint8_t getNodeForOperation(const FsContext &context,
	                                    const FilesystemOperationContext &fsOpContext,
	                                    ExpectedNodeType expectedNodeType, uint8_t modeMask,
	                                    inode_t inode, FSNode **nodeOut,
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
	/// Core node lookup operation - override in subclasses for custom storage.
	/// @param inode The inode of the node to look up.
	/// @return Pointer to the node if found, nullptr otherwise.
	virtual FSNode *idToNodeInternal(inode_t inode) const = 0;

	/// Core node lookup operation with context - override in subclasses for custom storage.
	/// @param context The FS context for the operation, potentially carrying a transaction.
	/// @param inode The inode of the node to look up.
	/// @return Pointer to the node if found, nullptr otherwise.
	virtual FSNode *idToNodeInternal(const FilesystemOperationContext &fsOpContext,
	                                 inode_t inode) const = 0;

	/// Increases the node counters for the specified type.
	/// @param fsOpContext The operation context carrying a transaction in some implementations.
	/// @param type The node type of whose related counters are to be updated.
	virtual void incrementNodeCounters(const FilesystemOperationContext &fsOpContext,
	                                   FSNodeType type) = 0;

	/// Implementation specific node preservation (in-memory, FDB, etc.)
	/// Usually called after creating or modifying a node to persist changes. An in-memory
	/// implementation most likely will add it to a hash map or similar structure. A KV-store
	/// implementation will write the node data to the store within the current transaction.
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param node The node to preserve.
	virtual void preserveNode(const FilesystemOperationContext &fsOpContext, FSNode *node) = 0;

	/// Implementation specific edge preservation (in-memory, FDB, etc.)
	/// Similar to preserveNode, but for edges (directory entries).
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param parent The parent directory node.
	/// @param child The child node.
	/// @param handlePtr The edge name.
	virtual void preserveEdge(const FilesystemOperationContext &fsOpContext,
	                          FSNodeDirectory *parent, FSNode *child,
	                          hstorage::Handle *handlePtr) = 0;

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
