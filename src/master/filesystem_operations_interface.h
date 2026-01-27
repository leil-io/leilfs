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
#include "master/filesystem_operation_context.h"
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

	/// Creates a filesystem operation context for the specified transaction type.
	/// In-memory implementation should return a context without transactions.
	virtual FilesystemOperationContext createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType type) = 0;

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
	virtual uint8_t append(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                       inode_t inode, inode_t inode_src) = 0;
	virtual uint8_t deleteAcl(const FsContext &context, inode_t inode, AclType type) = 0;

	/// Creates a hard link to an existing file in a destination directory.
	///
	/// This method creates a new directory entry (hard link) in the destination parent directory
	/// that points to an existing source node (file, symlink, device, etc.). Hard links allow
	/// multiple names to reference the same inode, enabling multiple paths to the same file.
	/// The operation should perform comprehensive validation, including:
	/// - Session verification (must be read-write, non-meta session)
	/// - Permission checks (write access required on destination parent)
	/// - Source node accessibility verification
	/// - Type validation (source cannot be a directory to maintain tree structure)
	/// - State validation (source cannot be in trash or reserved)
	/// - Name validation and uniqueness check in destination
	///
	/// After successful validation, the link is created by calling nodeOperations_->link(),
	/// which increments the node's link count, updates parent statistics, and propagates
	/// changes up the directory tree.
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The extra operation context (transaction in some implementations).
	/// @param inode_src The inode number of the existing node to link to (source).
	/// @param parent_dst The inode number of the destination parent directory where the link
	///                   will be created.
	/// @param name_dst The name for the new directory entry (link name) in the destination parent.
	/// @param[out] inode Pointer to inode_t where the source inode number will be stored.
	///                   Can be nullptr if not needed.
	/// @param[out] attr Pointer to Attributes to be filled with the source node's attributes
	///                  after the link operation. Can be nullptr if not needed.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - SAUNAFS_ERROR_EROFS if the session is read-only
	///         - SAUNAFS_ERROR_ENOENT if the session is meta-only or the source node doesn't
	///           exist or is in trash/reserved
	///         - SAUNAFS_ERROR_ENOTDIR if parent_dst is not a directory
	///         - SAUNAFS_ERROR_EACCES if write permission denied on destination parent
	///         - SAUNAFS_ERROR_EPERM if source is a directory
	///         - SAUNAFS_ERROR_EINVAL if name_dst validation fails
	///         - SAUNAFS_ERROR_EEXIST if name_dst already exists in destination parent
	///         - Other error codes as returned by node operations
	///
	/// @note Directories cannot have multiple hard links (to maintain tree structure).
	/// @note The source node's link count is incremented.
	/// @note Statistics are propagated up through all ancestor directories.
	virtual uint8_t link(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                     inode_t inode_src, inode_t parent_dst, const HString &name_dst,
	                     inode_t *inode, Attributes *attr) = 0;

	virtual uint8_t purge(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                      inode_t inode) = 0;

	/// Renames (moves) a filesystem node from one location to another.
	///
	/// This method performs an atomic rename operation, moving a node from the source
	/// location (parent_src/name_src) to the destination location (parent_dst/name_dst).
	/// If a node already exists at the destination, it will be unlinked first (subject
	/// to validation). The operation handles various constraints including:
	/// - Directory ancestry checks (cannot move a directory into its own subtree)
	/// - Quota validation (checks if the operation would exceed quota limits)
	/// - Sticky bit permissions on both source and destination
	/// - Case-insensitive filesystem support
	/// - Non-empty directory collision prevention
	///
	/// The actual rename is performed by: unlinking the destination (if exists),
	/// removing the edge from source parent, and creating a new link in the destination parent.
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The extra operation context carrying a transaction in some
	/// implementations.
	/// @param parent_src The inode number of the source parent directory.
	/// @param name_src The name of the node in the source directory.
	/// @param parent_dst The inode number of the destination parent directory.
	/// @param name_dst The desired name in the destination directory.
	/// @param[in,out] inode Pointer to inode_t. On input (for shadow/metarestore), the expected
	///                      inode to verify. On output, contains the inode of the renamed node.
	/// @param[out] attr Optional pointer to Attributes to be filled with the node's attributes
	///                  after the rename operation.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - SAUNAFS_ERROR_EINVAL if name validation fails or trying to move directory into
	///           its own subtree
	///         - SAUNAFS_ERROR_ENOENT if source node doesn't exist
	///         - SAUNAFS_ERROR_EPERM if sticky bit access check fails
	///         - SAUNAFS_ERROR_ENOTEMPTY if destination is a non-empty directory
	///         - SAUNAFS_ERROR_QUOTA if quota limits would be exceeded
	///         - SAUNAFS_ERROR_MISMATCH if inode doesn't match (shadow/metarestore only)
	///         - Other error codes as returned by node operations
	///
	/// @note If source and destination are the same, returns SAUNAFS_STATUS_OK immediately.
	/// @note Logs the operation as "MOVE" in the changelog for master personality.
	virtual uint8_t rename(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                       inode_t parent_src, const HString &name_src, inode_t parent_dst,
	                       const HString &name_dst, inode_t *inode, Attributes *attr) = 0;
	virtual uint8_t release(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                        inode_t inode, uint32_t sessionid) = 0;
	virtual uint8_t setExtraAttr(const FsContext &context, inode_t inode, uint8_t eattr,
	                             uint8_t smode, inode_t *sinodes, inode_t *ncinodes,
	                             inode_t *nsinodes) = 0;
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

	/// Creates a symbolic link (symlink) in the filesystem.
	///
	/// This method creates a new symbolic link with the specified name in the given parent
	/// directory that points to the provided target path. Symbolic links are special files
	/// that contain a reference to another file or directory path. Unlike hard links, symlinks
	/// can point to non-existent targets (dangling links), directories, and files across
	/// different filesystems. The operation performs comprehensive validation including:
	/// - Session verification (must be read-write, non-meta session)
	/// - Write permission check on the parent directory
	/// - Name validation and uniqueness check in the parent
	/// - On master personalities, quota verification (ensures inode quota limits are not exceeded)
	/// - Path validation (empty paths and paths containing null bytes are rejected)
	/// - For case-insensitive filesystems, attempts to canonicalize the target path
	///   (but allows dangling links if the target cannot be resolved)
	///
	/// After successful validation, a new symlink node is created with standard permissions
	/// (0777 by default), and its statistics (including path length) are propagated up
	/// through all ancestor directories.
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param parent The inode number of the parent directory where the symlink will be created.
	/// @param name The name of the new symlink within the parent directory.
	/// @param path The target path that the symlink will point to. Can be relative or absolute.
	///             The path is stored as-is and is not required to exist at creation time.
	/// @param[in,out] inode Pointer to inode_t.
	///                      - On master, must be set to 0 on input (will be assigned).
	///                      - On shadow/metarestore, must be set to the expected inode value from
	///                        the changelog. If the created inode does not match,
	///                        SAUNAFS_ERROR_MISMATCH should be returned.
	///                      On output, contains the inode number of the created symlink.
	/// @param[out] attr Optional pointer to Attributes to be filled with the symlink's attributes
	///                  after creation. Can be nullptr if not needed.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - SAUNAFS_ERROR_EROFS if the session is read-only
	///         - SAUNAFS_ERROR_ENOENT if the session is meta-only or parent doesn't exist
	///         - SAUNAFS_ERROR_ENOTDIR if parent is not a directory
	///         - SAUNAFS_ERROR_EACCES if write permission denied on parent directory
	///         - SAUNAFS_ERROR_EINVAL if name validation fails, path is empty, or path contains
	///           null bytes
	///         - SAUNAFS_ERROR_EEXIST if name already exists in parent directory
	///         - SAUNAFS_ERROR_QUOTA if quota limits would be exceeded
	///         - SAUNAFS_ERROR_MISMATCH if inode doesn't match expected value (shadow/metarestore
	///           only)
	///         - Other error codes as returned by node operations
	///
	/// @note Unlike hard links, symbolic links can point to directories and non-existent paths.
	/// @note The symlink's path length contributes to the parent directory's statistics.
	/// @note For case-insensitive filesystems, the target path is canonicalized when possible,
	///       but dangling symlinks are still permitted.
	virtual uint8_t symlink(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                        inode_t parent, const HString &name, const std::string &path,
	                        inode_t *inode, Attributes *attr) = 0;

	virtual uint8_t undel(const FsContext &context, inode_t inode) = 0;
	virtual uint8_t writeChunk(const FsContext &context,
	                           const FilesystemOperationContext &fsOpContext, inode_t inode,
	                           uint32_t index, bool usedummylockid,
	                           /* inout */ uint32_t *lockid, uint64_t *chunkid, uint8_t *opflag,
	                           uint64_t *length, uint32_t min_server_version = 0) = 0;
	virtual uint8_t setNextChunkId(const FsContext &context, uint64_t nextChunkId) = 0;

	/// Given a string representing a path, resolves and returns the canonical
	/// name as actually stored in the filesystem, matching the true case and
	/// spelling of each component. This is useful for implementing features
	/// such as case-insensitive filesystem operations, where the input path
	/// may differ in case or form from the stored names.
	virtual uint8_t getCanonicalPath(const FsContext &context,
	                                 const FilesystemOperationContext &fsOpContext,
	                                 const std::string &inputPath, std::string &canonicalPath) = 0;

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
	virtual uint8_t lookup(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                       inode_t parent, const HString &name, inode_t *inode,
	                       Attributes &attr) = 0;
	virtual uint8_t wholePathLookup(const FsContext &context, inode_t parent,
	                                const std::string &path, inode_t *found_inode,
	                                Attributes &attr) = 0;
	virtual uint8_t getAttr(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                        inode_t inode, Attributes &attr) = 0;
	virtual uint8_t trySetLength(const FsContext &context,
	                             const FilesystemOperationContext &fsOpContext, inode_t inode,
	                             uint8_t opened, uint64_t length, bool denyTruncatingParity,
	                             uint32_t lockid, Attributes &attr, uint64_t *chunkid) = 0;
	virtual uint8_t doSetLength(const FsContext &context,
	                            const FilesystemOperationContext &fsOpContext, inode_t inode,
	                            uint64_t length, Attributes &attr) = 0;
	virtual uint8_t setAttr(const FsContext &context, inode_t inode, uint8_t setmask,
	                        uint16_t attrmode, uint32_t attruid, uint32_t attrgid,
	                        uint32_t attratime, uint32_t attrmtime, SugidClearMode sugidclearmode,
	                        Attributes &attr) = 0;
	virtual uint8_t readlink(const FsContext &context, inode_t inode, std::string &path) = 0;
	virtual void statfs(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                    uint64_t *totalspace, uint64_t *availspace, uint64_t *trashspace,
	                    uint64_t *reservedspace, inode_t *inodes) = 0;

	/// Creates a filesystem node (file, socket, FIFO, or device).
	///
	/// Creates a new node of the specified type in the given parent directory.
	/// The function performs comprehensive validation including session verification,
	/// node type checking, quota verification, and name uniqueness.
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The extra operation context carrying a transaction in some
	/// implementations.
	/// @param parent The inode number of the parent directory.
	/// @param name The name of the new node within the parent directory.
	/// @param type The type of node to create (kFile, kSocket, kFifo, kBlockDev, or kCharDev).
	/// @param mode The file mode/permissions for the new node.
	/// @param umask The umask to apply when creating the node.
	/// @param rdev The device number for block/character devices (ignored for other types).
	/// @param inode Pointer to inode_t where the created node's inode number will be stored.
	/// @param attr Reference to Attributes structure to be filled with the new node's attributes.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - SAUNAFS_ERROR_EINVAL if type is invalid or name verification fails
	///         - SAUNAFS_ERROR_EEXIST if a node with the given name already exists in the parent
	///         - SAUNAFS_ERROR_QUOTA if quota limits would be exceeded
	///         - SAUNAFS_ERROR_EPERM if session permissions are insufficient
	///         - Other error codes as returned by node operations
	virtual uint8_t mknod(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                      inode_t parent, const HString &name, FSNodeType type, uint16_t mode,
	                      uint16_t umask, uint32_t rdev, inode_t *inode, Attributes &attr) = 0;

	/// Creates a new directory in the filesystem.
	///
	/// Creates a new directory with the specified name in the given parent directory.
	/// The function performs comprehensive validation including session verification,
	/// write permission checking, quota verification, name uniqueness, and optional
	/// disk space checks. The directory inherits ACLs from the parent if applicable.
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The extra operation context carrying a transaction in some
	/// implementations.
	/// @param parent The inode number of the parent directory.
	/// @param name The name of the new directory within the parent directory.
	/// @param mode The file mode/permissions for the new directory.
	/// @param umask The umask to apply when creating the directory.
	/// @param copysgid If non-zero, copies the SGID bit from parent directory.
	/// @param inode Pointer to inode_t where the created directory's inode number will be stored.
	/// @param attr Reference to Attributes to be filled with the new directory's attributes.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - SAUNAFS_ERROR_EINVAL if name verification fails
	///         - SAUNAFS_ERROR_ENOTDIR if parent is not a directory
	///         - SAUNAFS_ERROR_EEXIST if a node with the given name already exists in the parent
	///         - SAUNAFS_ERROR_QUOTA if quota limits would be exceeded
	///         - SAUNAFS_ERROR_NOSPACE if disk space is depleted (when
	///         gDisableEmptyFoldersMetadataOnFullDisk is enabled)
	///         - SAUNAFS_ERROR_EPERM if session permissions are insufficient
	///         - Other error codes as returned by node operations
	virtual uint8_t mkdir(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                      inode_t parent, const HString &name, uint16_t mode, uint16_t umask,
	                      uint8_t copysgid, inode_t *inode, Attributes &attr) = 0;
	virtual uint8_t removeChunkFromFile(const FsContext &context,
	                                    const FilesystemOperationContext &fsOpContext,
	                                    inode_t inode, uint64_t chunkId) = 0;
	virtual uint8_t repair(const FsContext &context, inode_t inode, uint8_t correct_only,
	                       uint32_t *notchanged, uint32_t *erased, uint32_t *repaired) = 0;

	/// Removes an empty directory from the filesystem.
	///
	/// This method removes a directory if and only if it is empty (contains no entries).
	/// The operation performs comprehensive validation including:
	/// - Session verification (must be a non-meta session)
	/// - Write permission check on the parent directory
	/// - Name validation
	/// - Node existence and type verification (must be a directory)
	/// - Sticky bit access control
	/// - Empty directory check (must have 0 entries)
	///
	/// If all checks pass, the directory is unlinked from its parent, which may move it
	/// to trash or delete it permanently depending on trashtime and session state.
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The extra operation context carrying a transaction in some
	///                    implementations.
	/// @param parent The inode number of the parent directory containing the directory to remove.
	/// @param name The name of the directory to remove.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - SAUNAFS_ERROR_EINVAL if name validation fails
	///         - SAUNAFS_ERROR_ENOENT if the directory doesn't exist
	///         - SAUNAFS_ERROR_ENOTDIR if the node exists but is not a directory
	///         - SAUNAFS_ERROR_ENOTEMPTY if the directory contains entries
	///         - SAUNAFS_ERROR_EPERM if sticky bit access check fails
	///         - Other error codes as returned by node operations
	///
	/// @note Logs the operation as "UNLINK" (not "RMDIR") in the changelog.
	/// @note The actual deletion is performed by calling unlink() on the node operations.
	virtual uint8_t rmdir(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                      inode_t parent, const HString &name) = 0;

	virtual uint8_t recursiveRemove(const FsContext &context, inode_t parent, const HString &name,
	                                const std::function<void(int)> &callback, uint32_t job_id) = 0;
	virtual uint8_t readdirSize(const FsContext &context, inode_t inode, uint8_t flags,
	                            void **dnode, uint32_t *dbuffsize) = 0;
	virtual void readdirData(const FsContext &context, uint8_t flags, void *dnode,
	                         uint8_t *dbuff) = 0;

	/// Reads a paginated list of entries from a directory.
	///
	/// Retrieves directory entries starting from a specified index, with support for
	/// pagination. Verifies session validity and read permissions before accessing
	/// the directory contents. Updates the directory's access time upon successful read.
	///
	/// @param context Session context containing user credentials (uid, gid), session
	///                flags, and root inode information.
	/// @param inode The inode number of the directory to read.
	/// @param first_entry Starting index for pagination (offset into the directory listing).
	/// @param number_of_entries Maximum number of entries to return.
	/// @param[out] dir_entries Output vector populated with directory entries, each
	///                         containing index, next_index, inode, name, and attributes.
	/// @return SAUNAFS_STATUS_OK on success, or an appropriate error code on failure
	///         (e.g., SAUNAFS_ERROR_ENOENT if directory not found,
	///         SAUNAFS_ERROR_EACCES if permission denied).
	virtual uint8_t readdir(const FsContext &context, inode_t inode, uint64_t first_entry,
	                        uint64_t number_of_entries,
	                        std::vector<DirectoryEntry> &dir_entries) = 0;

	virtual uint8_t checkFile(const FsContext &context, inode_t inode,
	                          ChunkCountArray &chunkCount) = 0;
	virtual uint8_t openCheck(const FsContext &context, inode_t inode, uint8_t flags,
	                          Attributes &attr) = 0;
	virtual uint8_t getGoal(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                        inode_t inode, uint8_t gmode, GoalStatistics &fgtab,
	                        GoalStatistics &dgtab) = 0;
	virtual uint8_t getExtraAttr(const FsContext &context, inode_t inode, uint8_t gmode,
	                             ExtraAttributesArray &fileEAttrTab,
	                             ExtraAttributesArray &dirEAttrTab) = 0;
	virtual uint8_t listXAttrLeng(const FsContext &context,
	                              const FilesystemOperationContext &fsOpContext, inode_t inode,
	                              uint8_t opened, void **xanode, uint32_t *xasize) = 0;
	virtual uint8_t getXAttr(const FsContext &context,
	                         const FilesystemOperationContext &fsOpContext, inode_t inode,
	                         uint8_t opened, uint8_t anleng, const uint8_t *attrname,
	                         uint32_t *avleng, uint8_t **attrvalue) = 0;
	virtual uint8_t setXAttr(const FsContext &context, inode_t inode, uint8_t opened,
	                         uint8_t anleng, const uint8_t *attrname, uint32_t avleng,
	                         const uint8_t *attrvalue, uint8_t mode) = 0;

	/// Removes (unlinks) a file or non-directory node from the filesystem.
	///
	/// This method removes a non-directory node (regular file, symlink, socket, FIFO, or device)
	/// from the specified parent directory. The operation performs comprehensive validation:
	/// - Session verification (must be a non-meta session)
	/// - Write permission check on the parent directory
	/// - Case-insensitive name resolution (if applicable)
	/// - Name validation
	/// - Node existence verification
	/// - Sticky bit access control
	/// - Directory rejection (cannot unlink directories - use rmdir instead)
	///
	/// If all checks pass, the node is unlinked. If this is the last link to the node,
	/// it may be moved to trash (if trashtime > 0), moved to reserved (if has open sessions),
	/// or deleted permanently.
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The extra operation context carrying a transaction in some
	///                    implementations.
	/// @param parent The inode number of the parent directory containing the node.
	/// @param name The name of the node to unlink.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - SAUNAFS_ERROR_EINVAL if name validation fails
	///         - SAUNAFS_ERROR_ENOENT if the node doesn't exist
	///         - SAUNAFS_ERROR_EPERM if the node is a directory or sticky bit access check fails
	///         - Other error codes as returned by node operations
	///
	/// @note This operation only works on non-directory nodes. Use rmdir() for directories.
	/// @note Logs the operation as "UNLINK" in the changelog.
	/// @note Supports case-insensitive filesystems with automatic name resolution.
	virtual uint8_t unlink(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                       inode_t parent, const HString &name) = 0;

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
	virtual uint8_t writeEnd(const FilesystemOperationContext &fsOpContext, inode_t inode,
	                         uint64_t length, uint64_t chunkid, uint32_t lockid) = 0;
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
	virtual uint8_t getChunkId(const FsContext &context,
	                           const FilesystemOperationContext &fsOpContext, inode_t inode,
	                           uint32_t index, uint64_t *chunkid) = 0;

	// SPECIAL - LOG EMERGENCY INCREASE VERSION FROM CHUNKS-MODULE
	virtual void increaseChunkVersion(uint64_t chunkid) = 0;

	virtual uint8_t fullPathByInode(const FsContext &context, inode_t inode,
	                                std::string &fullPath) = 0;
	virtual std::string fullPathByInode(inode_t initialInode) = 0;

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
	virtual uint8_t applyAttr(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                          inode_t inode, uint32_t mode, uint32_t uid, uint32_t gid,
	                          uint32_t atime, uint32_t mtime) = 0;
	virtual uint8_t applySession(uint32_t sessionid) = 0;
	virtual uint8_t applyIncreaseChunkVersion(uint64_t chunkid) = 0;
	virtual uint8_t applyLength(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                            inode_t inode, uint64_t length, bool eraseFurtherChunks) = 0;
	virtual uint8_t applyRepair(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                            inode_t inode, uint32_t indx, uint32_t nversion) = 0;
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
