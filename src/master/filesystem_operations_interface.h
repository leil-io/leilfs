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

#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/attributes.h"
#include "common/defective_file_info.h"
#include "common/goal.h"
#include "errors/saunafs_error_codes.h"
#include "master/checksum.h"
#include "master/filesystem_node_operations_interface.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_operation_context.h"
#include "master/fs_context.h"
#include "master/locks.h"
#include "master/setgoal_task.h"
#include "master/settrashtime_task.h"
#include "protocol/quota.h"

class HString;
class AccessControlList;
class RichACL;

struct DirectoryEntry;
struct ChunkWithAddressAndLabel;

struct QuotaEntry;
struct QuotaOwner;

/// Result of a quota enforcement check. Fallible backends must report quota read failures
/// through status, because a KV backend read error may not reliably fail the later commit.
struct QuotaCheckResult {
	uint8_t status = SAUNAFS_STATUS_OK;
	bool exceeded = false;
};

inline uint8_t statusFromQuotaCheck(const QuotaCheckResult &result) {
	if (result.status != SAUNAFS_STATUS_OK) { return result.status; }
	return result.exceeded ? SAUNAFS_ERROR_QUOTA : SAUNAFS_STATUS_OK;
}

struct NamedInodeEntry;
struct HandleInodeEntry;

inline constexpr char kAclXattrs[] = "system.richacl";

// Sentinel strings written by getDirPathData() for error cases
inline constexpr std::string_view kDirPathNotFound = "(not found)";
inline constexpr std::string_view kDirPathNotDirectory = "(not directory)";

/// Result of a getXAttr operation. Owns the attribute value bytes.
struct XAttrGetResult {
	std::vector<uint8_t> value;
};

/// Result of a listXAttr operation. Owns the serialized name list.
struct XAttrListResult {
	/// Serialized xattr names: each name is followed by a '\0' byte.
	/// Does NOT include the always-present kAclXattrs prefix.
	std::vector<uint8_t> data;
};

/// Internal carrier for file-test status returned by fs_test_getdata().
struct FsTestReport {
	uint32_t loopStart = 0;
	uint32_t loopEnd = 0;
	inode_t files = 0;
	inode_t underGoalFiles = 0;
	inode_t missingFiles = 0;
	uint32_t chunks = 0;
	uint32_t underGoalChunks = 0;
	uint32_t missingChunks = 0;
	std::string report;
};

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

	/// Increments the metadata version counter and returns the pre-increment value (the version of
	/// this mutation).
	virtual uint64_t increaseMetadataVersion(const FilesystemOperationContext &fsOpContext) = 0;

	/// Adds an entry to a changelog, updates filesystem.cc internal structures, prepends a
	/// proper timestamp to changelog entry and broadcasts it to metaloggers and shadow masters.
	/// The __attribute__ is used to ensure printf-like format string checking by the compiler.
	/// @param fsOpContext Operation context (provides transaction for KV backends).
	/// @param ts Timestamp to add to the changelog entry.
	/// @param format printf-like format string for the changelog entry.
	/// @param ... Variable arguments for the format string.
	virtual void changeLog(const FilesystemOperationContext &fsOpContext, uint32_t ts,
	                       const char *format, ...)
	    __attribute__((__format__(__printf__, 4, 5))) = 0;

	/// Retrieves the stored path for a detached trash/reserved node.
	/// Implementations may override this to load detached paths from backend-specific storage.
	virtual std::optional<std::string> getDetachedPath(
	    const FilesystemOperationContext &fsOpContext, const FSNode *node) = 0;

	// Functions which create/apply (depending on the given context) changes to the metadata.
	// Common for metarestore and master server (both personalities)

	/// Registers a session as having acquired (opened) a file.
	///
	/// This method adds the specified session ID to the file's list of sessions that have
	/// the file open. It is called when a client opens a file and is used to track file
	/// usage across sessions. The operation ensures that:
	/// - The target inode exists and is a valid file node (regular file, trash, or reserved)
	/// - The session has not already acquired this file (prevents duplicate acquisitions)
	///
	/// For shadow personalities, this operation also updates the internal open file tracking
	/// used by the metalogger service (matoclserv_add_open_file).
	///
	/// The operation updates the file node's checksum and logs the change to the changelog
	/// on master personalities (as "ACQUIRE").
	///
	/// @param context The FS operation context containing user credentials, session info,
	///                and timestamp.
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param inode The inode number of the file to acquire.
	/// @param sessionid The session identifier that is acquiring the file.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - SAUNAFS_ERROR_ENOENT if the inode does not exist
	///         - SAUNAFS_ERROR_EPERM if the inode is not a file, trash, or reserved node
	///         - SAUNAFS_ERROR_EINVAL if the session has already acquired this file
	///
	/// @note This operation must be paired with a corresponding release() call when the
	///       file is closed.
	/// @note Multiple sessions can acquire the same file simultaneously.
	virtual uint8_t acquire(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                        inode_t inode, uint32_t sessionid) = 0;

	/// Unregisters a session that has released (closed) a file.
	///
	/// This method removes the specified session ID from the file's list of sessions that
	/// have the file open. It is called when a client closes a file and is the counterpart
	/// to the acquire() operation. The operation:
	/// - Verifies the target inode exists and is a valid file node (regular file, trash,
	///   or reserved)
	/// - Removes the session ID from the file's session list
	/// - For reserved files with no remaining sessions, automatically purges the file
	///   (deletes it permanently)
	/// - Updates the file node's checksum
	///
	/// For shadow personalities, this operation also updates the internal open file tracking
	/// used by the metalogger service (matoclserv_remove_open_file).
	///
	/// The operation logs the change to the changelog on master personalities (as "RELEASE").
	///
	/// @param context The FS operation context containing user credentials, session info,
	///                and timestamp.
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param inode The inode number of the file to release.
	/// @param sessionid The session identifier that is releasing the file.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - SAUNAFS_ERROR_ENOENT if the inode does not exist
	///         - SAUNAFS_ERROR_EPERM if the inode is not a file, trash, or reserved node
	///         - SAUNAFS_ERROR_EINVAL if the session was not found in the file's session list
	///
	/// @note This operation should only be called after a successful acquire() for the same
	///       session and inode.
	/// @note Reserved files are automatically purged when their last session is released.
	virtual uint8_t release(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                        inode_t inode, uint32_t sessionid) = 0;

	/// Appends the contents of one file to another.
	///
	/// Interprets `inode` as destination and `inode_src` as source.
	/// On success, destination file data is extended by the source file data in order.
	/// If the source file has no chunks, no data is copied, but all preceding validations
	/// (session, permissions, quota) still apply.
	///
	/// @param context The FS operation context containing user credentials, session info,
	///                and timestamp.
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param inode Destination inode (must be writable in the current session context).
	/// @param inode_src Source inode (must be readable in the current session context).
	///
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_EINVAL if `inode == inode_src`.
	/// @return SAUNAFS_ERROR_EROFS if the session is read-only.
	/// @return SAUNAFS_ERROR_ENOENT if the session is invalid for this operation or an inode
	///         cannot be resolved.
	/// @return SAUNAFS_ERROR_EPERM if an inode is outside the session-visible subtree or is not
	///         a file-like node.
	/// @return SAUNAFS_ERROR_EACCES if read/write permissions are insufficient.
	/// @return SAUNAFS_ERROR_QUOTA if appending would exceed quota (master personality).
	/// @return SAUNAFS_ERROR_INDEXTOOBIG if the resulting chunk index range would overflow.
	virtual uint8_t append(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                       inode_t inode, inode_t inode_src) = 0;

	/// Removes or prunes the ACL stored on a node, given its inode.
	///
	/// Verifies the session (read-write, non-meta) and resolves the inode to a node, then
	/// delegates to IFilesystemNodeOperations::deleteAcl. On success, writes a
	/// DELETEACL(inode, type_char) entry to the changelog (master personality) or
	/// increments the metadata version (shadow personality).
	///
	/// @param context The FS operation context (user credentials, session flags, timestamp).
	/// @param fsOpContext The operation context carrying a read-write transaction in KV backends.
	/// @param inode The inode of the node whose ACL to delete or prune.
	/// @param type The ACL type to remove: kRichACL removes the entire ACL; kDefault removes
	///             default (inherit-only) entries; kAccess removes access entries.
	/// @return SAUNAFS_STATUS_OK on success, or a session/permission/node-lookup error.
	virtual uint8_t deleteAcl(const FsContext &context,
	                          const FilesystemOperationContext &fsOpContext, inode_t inode,
	                          AclType type) = 0;

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

	/// Purges a trash node from metadata.
	///
	/// The inode must resolve to a trash-type node; otherwise the call fails.
	/// For contexts with session data, this operation requires a meta session.
	///
	/// On success, the trash entry is removed. If the node still has active file sessions,
	/// it is moved to reserved instead of being removed immediately, while still reporting
	/// success.
	///
	/// @param context The FS operation context containing user credentials, session info,
	///                and timestamp.
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param inode The inode of the trash node to purge.
	///
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_EROFS if the session is read-only.
	/// @return SAUNAFS_ERROR_EPERM if the session is not a meta session, or the inode
	///         resolves to a node that is inaccessible from a meta session.
	/// @return SAUNAFS_ERROR_ENOENT if the inode does not exist or is not a trash-type node.
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

	/// Updates extra-attribute flags on a node (optionally recursively).
	///
	/// Applies `eattr` using `smode` semantics (set/increase/decrease, with optional recursion).
	/// Valid bits in `eattr` are: `EATTR_NOOWNER`, `EATTR_NOACACHE`, `EATTR_NOECACHE`,
	/// and `EATTR_NODATACACHE`.
	///
	/// Counter parameters:
	/// - On master, outputs the number of changed (`sinodes`), unchanged (`ncinodes`),
	///   and permission-denied (`nsinodes`) inodes.
	/// - On shadow/restore replay, inputs are expected counts from changelog replay and
	///   are validated against the locally computed values.
	///
	/// @param context The FS operation context (credentials/session/timestamp).
	/// @param fsOpContext The operation context carrying a read-write transaction in KV backends.
	/// @param inode Root inode for the operation.
	/// @param eattr Extra-attribute bitmask to apply.
	/// @param smode Mode controlling set/increase/decrease and recursion.
	/// @param[in,out] sinodes Changed count (output on master; expected input on shadow/restore).
	/// @param[in,out] ncinodes Unchanged count (output on master; expected input on
	/// shadow/restore).
	/// @param[in,out] nsinodes Permission-denied count (output on master; expected input on
	/// shadow/restore).
	///
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_EINVAL if `smode` or `eattr` is invalid.
	/// @return SAUNAFS_ERROR_EROFS if the session is read-only.
	/// @return SAUNAFS_ERROR_ENOENT if the inode cannot be resolved or the session context
	///         is invalid for this non-meta operation.
	/// @return SAUNAFS_ERROR_EPERM if the target is outside the session-visible namespace, or
	///         (non-recursive mode) no change is permitted due to ownership checks.
	/// @return SAUNAFS_ERROR_MISMATCH on shadow/restore replay if expected counters differ.
	virtual uint8_t setExtraAttr(const FsContext &context,
	                             const FilesystemOperationContext &fsOpContext, inode_t inode,
	                             uint8_t eattr, uint8_t smode, inode_t *sinodes, inode_t *ncinodes,
	                             inode_t *nsinodes) = 0;

	/// Schedules setting a storage goal on a node (optionally recursively).
	///
	/// Accepted `smode` values are `SMODE_SET` and `SMODE_RSET` only.
	/// `setgoal_stats` accumulates changed/not-changed/not-permitted counters.
	///
	/// If the operation is not finished in the initial batch, returns
	/// `SAUNAFS_ERROR_WAITING`; in that case `callback` is invoked on completion with the
	/// final status. If it finishes immediately, `callback` is not invoked.
	///
	/// @param context The FS operation context (credentials/session/timestamp).
	/// @param inode Root inode for the operation.
	/// @param goal Target goal id.
	/// @param smode Set mode (`SMODE_SET` or `SMODE_RSET`).
	/// @param setgoal_stats Shared counters for changed/not-changed/not-permitted inodes.
	/// @param callback Completion callback used when asynchronous processing is required.
	///
	/// @return SAUNAFS_STATUS_OK on immediate success.
	/// @return SAUNAFS_ERROR_WAITING if accepted for asynchronous completion.
	/// @return SAUNAFS_ERROR_EINVAL if `goal` or `smode` is invalid.
	/// @return SAUNAFS_ERROR_EROFS if the session is read-only.
	/// @return SAUNAFS_ERROR_ENOENT if the inode cannot be resolved.
	/// @return SAUNAFS_ERROR_EPERM if the target type is unsupported (not a directory,
	///         file, trash, or reserved node), the target is outside the session-visible
	///         namespace, or (non-recursive mode) a single-node owner check fails and the
	///         initial batch completes immediately. In recursive mode, owner-based denials
	///         are accumulated in `setgoal_stats` counters instead.
	virtual uint8_t setGoal(const FsContext &context, inode_t inode, uint8_t goal, uint8_t smode,
	                        std::shared_ptr<SetGoalTask::StatsArray> setgoal_stats,
	                        const std::function<void(int)> &callback) = 0;

	/// Applies a single-node goal update during shadow/restore replay and verifies consistency.
	///
	/// Computes the local per-node set-goal result for `inode` and compares it with
	/// `master_result` (expected `SetGoalTask::k*` result code).
	///
	/// @param context The FS operation context (typically shadow/restore replay context).
	/// @param inode The inode to update.
	/// @param goal Target goal id.
	/// @param smode Set mode (`SMODE_SET` or `SMODE_RSET`).
	/// @param master_result Expected per-node result code from master changelog replay.
	///
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_EINVAL if `goal` or `smode` is invalid.
	/// @return SAUNAFS_ERROR_EROFS if the session is read-only.
	/// @return SAUNAFS_ERROR_ENOENT if the inode cannot be resolved.
	/// @return SAUNAFS_ERROR_EPERM if the target type is unsupported (not a directory,
	///         file, trash, or reserved node) or the target is outside the session-visible
	///         namespace.
	/// @return SAUNAFS_ERROR_MISMATCH if local result differs from `master_result`.
	virtual uint8_t applySetGoal(const FsContext &context, inode_t inode, uint8_t goal,
	                             uint8_t smode, uint32_t master_result) = 0;

	/// Updates the stored path string of a trash inode.
	///
	/// For contexts with session data, this operation requires a meta session.
	/// `path` must be non-empty and must not contain embedded `'\0'` bytes.
	///
	/// @param context The FS operation context (credentials/session/timestamp).
	/// @param inode Inode expected to be in trash.
	/// @param path New stored trash path.
	///
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_EINVAL if `path` is empty or contains `'\0'`.
	/// @return SAUNAFS_ERROR_EROFS if the session is read-only.
	/// @return SAUNAFS_ERROR_EPERM if the session is not a meta session, or inode is outside
	///         the session-visible namespace.
	/// @return SAUNAFS_ERROR_ENOENT if inode cannot be resolved as a trash node.
	virtual uint8_t setTrashPath(const FsContext &context, inode_t inode,
	                             const std::string &path) = 0;

	/// Schedules setting trash-time on a node (optionally recursively).
	///
	/// `smode` controls set/increase/decrease semantics and optional recursion.
	/// `settrashtime_stats` accumulates changed/not-changed/not-permitted counters.
	///
	/// If the operation is not finished in the initial batch, returns
	/// `SAUNAFS_ERROR_WAITING`; in that case `callback` is invoked on completion with the
	/// final status. If it finishes immediately, `callback` is not invoked.
	///
	/// @param context The FS operation context (credentials/session/timestamp).
	/// @param inode Root inode for the operation.
	/// @param trashtime Target trash-time value.
	/// @param smode Mode controlling set/increase/decrease and recursion.
	/// @param settrashtime_stats Shared counters for changed/not-changed/not-permitted inodes.
	/// @param callback Completion callback used when asynchronous processing is required.
	///
	/// @return SAUNAFS_STATUS_OK on immediate success.
	/// @return SAUNAFS_ERROR_WAITING if accepted for asynchronous completion.
	/// @return SAUNAFS_ERROR_EINVAL if `smode` is invalid.
	/// @return SAUNAFS_ERROR_EROFS if the session is read-only.
	/// @return SAUNAFS_ERROR_ENOENT if the inode cannot be resolved.
	/// @return SAUNAFS_ERROR_EPERM if the target type is unsupported (not a directory,
	///         file, trash, or reserved node), the target is outside the session-visible
	///         namespace, or (non-recursive mode) a single-node owner check fails and the
	///         initial batch completes immediately. In recursive mode, owner-based denials
	///         are accumulated in `settrashtime_stats` counters instead.
	virtual uint8_t setTrashTime(const FsContext &context, inode_t inode, uint32_t trashtime,
	                             uint8_t smode,
	                             std::shared_ptr<SetTrashtimeTask::StatsArray> settrashtime_stats,
	                             const std::function<void(int)> &callback) = 0;

	/// Applies a single-node trash-time update on shadow/restore replay and verifies consistency.
	///
	/// Computes the local per-node set-trashtime result for `inode` and compares it with
	/// `master_result` (expected `SetTrashtimeTask::k*` result code).
	///
	/// @param context The FS operation context (typically shadow/restore replay context).
	/// @param inode The inode to update.
	/// @param trashtime Target trash-time value.
	/// @param smode Mode controlling set/increase/decrease and recursion.
	/// @param master_result Expected per-node result code from master changelog replay.
	///
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_EINVAL if `smode` is invalid.
	/// @return SAUNAFS_ERROR_EROFS if the session is read-only.
	/// @return SAUNAFS_ERROR_ENOENT if the inode cannot be resolved.
	/// @return SAUNAFS_ERROR_EPERM if the target type is unsupported (not a directory,
	///         file, trash, or reserved node) or the target is outside the session-visible
	///         namespace.
	/// @return SAUNAFS_ERROR_MISMATCH if local result differs from `master_result`.
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

	/// Restores a file from trash using the path stored with its detached metadata.
	///
	/// The operation is intended for meta sessions. It resolves `inode`, verifies that it is a
	/// trash node, validates the stored trash path, reconstructs the destination path from the
	/// filesystem root, creates missing intermediate directories when needed, and links the file
	/// back as an active file. The final path component must not already exist.
	///
	/// On master personalities, a successful restore is recorded as `UNDEL(inode)`. On replay
	/// personalities, the operation applies that changelog entry and advances metadata state.
	/// Implementations that need transactions create and commit their operation context internally.
	///
	/// @param context The FS operation context containing session data and timestamp.
	/// @param inode The inode of the trash file to restore.
	///
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_EROFS if the session is read-only.
	/// @return SAUNAFS_ERROR_EPERM if the session is not a meta session or cannot access the
	///         inode.
	/// @return SAUNAFS_ERROR_ENOENT if the inode does not exist or is not a trash node.
	/// @return SAUNAFS_ERROR_CANTCREATEPATH if the stored path is invalid or cannot be
	///         reconstructed.
	/// @return SAUNAFS_ERROR_EEXIST if the destination path already exists.
	/// @return SAUNAFS_ERROR_IO if a transactional backend cannot commit the restore.
	virtual uint8_t undel(const FsContext &context, inode_t inode) = 0;

	/// Prepares a file chunk for client-side writing.
	///
	/// The operation validates a read-write, non-meta session, resolves `inode` as a file-like
	/// node, verifies `index`, and prepares the chunk at that index for modification. Depending on
	/// the current file layout and chunk state, this may create a new chunk, lock an existing
	/// chunk, duplicate a shared chunk, or start a version-increase operation before the client
	/// writes data. The actual file length is finalized later by writeEnd().
	///
	/// On success, both personalities update file layout, timestamps, checksum, and quota
	/// counters. Master additionally logs `WRITE(inode,index,version_flag,lockid):chunkid`; on
	/// replay, `chunkid` is the expected resulting chunk id and mismatches return
	/// SAUNAFS_ERROR_MISMATCH.
	///
	/// @param context The FS operation context containing session data and timestamp.
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param inode The inode of the file-like node to write.
	/// @param index The chunk index in the file.
	/// @param[in,out] lockid Existing lock id to verify, or 0 to request/receive a lock id.
	/// @param[in,out] chunkid On master, receives the resulting chunk id; on replay,
	///                        contains the expected resulting chunk id and is updated on
	///                        success. If the chunk is already locked, receives the locked
	///                        chunk id.
	/// @param[in,out] opflag On master, receives the chunk operation code; non-zero means the
	///                       caller must wait for chunkserver-side work. On replay, contains the
	///                       changelog flag that controls whether the chunk version is increased.
	/// @param[out] length Optional output for the current file length; may be nullptr.
	/// @param min_server_version Min chunkserver version for new chunk allocation (master only).
	///
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_EROFS if the session is read-only.
	/// @return SAUNAFS_ERROR_ENOENT if the inode cannot be resolved.
	/// @return SAUNAFS_ERROR_EPERM if the inode is not file-like or the session is invalid.
	/// @return SAUNAFS_ERROR_INDEXTOOBIG if `index` exceeds the maximum chunk index.
	/// @return SAUNAFS_ERROR_QUOTA if allocating or duplicating the chunk would exceed quota.
	/// @return SAUNAFS_ERROR_LOCKED if the chunk is locked by another operation.
	/// @return SAUNAFS_ERROR_NOTLOCKED or SAUNAFS_ERROR_WRONGLOCKID if the supplied lock id does
	///         not match current chunk state.
	/// @return SAUNAFS_ERROR_MISMATCH on replay when the resulting chunk id differs from
	///         `chunkid`.
	/// @return Other chunk allocation/modification errors such as SAUNAFS_ERROR_NOCHUNKSERVERS,
	///         SAUNAFS_ERROR_NOSPACE, SAUNAFS_ERROR_NOCHUNK, SAUNAFS_ERROR_CHUNKLOST, or
	///         SAUNAFS_ERROR_CHUNKBUSY.
	///
	/// @see writeEnd
	virtual uint8_t writeChunk(const FsContext &context,
	                           const FilesystemOperationContext &fsOpContext, inode_t inode,
	                           uint32_t index,
	                           /* inout */ uint32_t *lockid, uint64_t *chunkid, uint8_t *opflag,
	                           uint64_t *length, uint32_t min_server_version = 0) = 0;

	/// Raises the next chunk-id floor used by strictly monotonic chunk-id generators.
	///
	/// Implementations must not lower the current next chunk id. Passing a value lower than the
	/// current one leaves metadata unchanged and returns SAUNAFS_ERROR_MISMATCH. Successful master
	/// calls are logged as `NEXTCHUNKID(nextChunkId)`, and replay personalities apply that entry to
	/// keep future allocations above already known chunk ids.
	///
	/// @param context The FS operation context containing timestamp and personality information.
	/// @param nextChunkId The minimum next chunk id to use for future allocations.
	///
	/// @return SAUNAFS_STATUS_OK if the next chunk-id floor was accepted.
	/// @return SAUNAFS_ERROR_MISMATCH if `nextChunkId` is lower than the current next chunk id.
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

	/// Checks whether the client session can access the file specified by inode.
	///
	/// This method verifies if the user associated with the client session has the requested
	/// access permissions (read, write, or execute) for a given filesystem node. It performs
	/// session validation and permission checking based on the user's credentials (uid/gid)
	/// and the requested access mode.
	///
	/// The function performs the following validations:
	/// - Session verification (must be a non-meta session)
	/// - Session mode check (read-write if write access is requested, otherwise read-only)
	/// - Node existence verification
	/// - Permission verification based on file mode, ACLs, and user credentials
	///
	/// The modemask parameter specifies what type of access to check:
	/// - MODE_MASK_R (4): Check read permission
	/// - MODE_MASK_W (2): Check write permission
	/// - MODE_MASK_X (1): Check execute permission
	/// - MODE_MASK_EMPTY (0): Check node existence only
	///
	/// These flags can be combined using bitwise OR to check multiple permissions.
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param inode The inode number of the node to check access for.
	/// @param modemask Bitmask specifying which access permissions to check (MODE_MASK_R,
	///                 MODE_MASK_W, MODE_MASK_X, or combinations thereof).
	///
	/// @return SAUNAFS_STATUS_OK if the user has the requested access, or one of the following
	///         error codes:
	///         - SAUNAFS_ERROR_EACCES if access is denied (insufficient permissions)
	///         - SAUNAFS_ERROR_ENOENT if the node doesn't exist
	///         - SAUNAFS_ERROR_EPERM if session permissions are insufficient
	///         - Other error codes as returned by node operations
	virtual uint8_t access(const FsContext &context, inode_t inode, int modemask) = 0;

	/// Looks up a child entry in a directory by name.
	///
	/// This method performs a directory lookup operation, retrieving the inode and attributes
	/// of a child entry within a parent directory. The function supports special directory
	/// entries ("." for current directory and ".." for parent directory) and handles case-sensitive
	/// or case-insensitive filesystem semantics.
	///
	/// The function performs the following validations:
	/// - Session verification (must be a non-meta session, operation is read-only)
	/// - Parent node verification (must exist and be a directory)
	/// - Execute permission check on the parent directory (required for directory traversal)
	/// - Name validation
	/// - Child entry lookup within the parent directory
	/// - Case-sensitive or case-insensitive name matching
	///
	/// Special handling for dot-entries:
	/// - "." (single dot): Returns the current directory inode, or SPECIAL_INODE_ROOT if at root
	/// - ".." (double dot): Returns the parent directory inode, or SPECIAL_INODE_ROOT if at root
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param parent The inode number of the parent directory to search in.
	/// @param name The name of the entry to look up within the parent directory.
	/// @param[out] inode Pointer to inode_t where the found entry's inode number will be stored.
	///                   Set to 0 on error.
	/// @param[out] attr Reference to Attributes structure to be filled with the found entry's
	///                  properties (permissions, ownership, timestamps, etc.).
	///
	/// @return SAUNAFS_STATUS_OK on successful lookup, or one of the following error codes:
	///         - SAUNAFS_ERROR_ENOENT if the child entry doesn't exist in the parent directory
	///         - SAUNAFS_ERROR_EINVAL if name validation fails
	///         - SAUNAFS_ERROR_ENOTDIR if parent is not a directory
	///         - SAUNAFS_ERROR_EACCES if access is denied (insufficient permissions in parent)
	///         - SAUNAFS_ERROR_EPERM if session permissions are insufficient
	///         - Other error codes as returned by node operations
	virtual uint8_t lookup(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                       inode_t parent, const HString &name, inode_t *inode,
	                       Attributes &attr) = 0;

	/// Looks up a node by traversing a multi-component path.
	///
	/// This method performs a multi-level directory traversal operation, resolving a path
	/// (containing zero or more '/' delimiters) starting from the provided `parent` inode.
	/// It decomposes the path into individual components and performs a sequential lookup
	/// for each non-empty component.
	///
	/// Path parsing / edge-case behavior (current implementation):
	/// - Empty components are ignored, so "dir1//dir2" is treated as "dir1/dir2".
	/// - A trailing '/' is ignored, so "dir1/" is treated as "dir1".
	/// - If `path` contains no non-empty components (e.g., "" or "/" or "///"), the function
	///   resolves to the session root inode (context.rootinode()) and returns root attributes
	///   via SPECIAL_INODE_ROOT (historical behavior).
	///
	/// The function performs the following validations (per component), delegated to `lookup()`:
	/// - Session verification
	/// - Parent node verification
	/// - Execute permission checks on parent directories
	/// - Name validation for each path component
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param parent The inode number of the starting directory for the path traversal.
	/// @param path The path to traverse, which may contain multiple '/' delimiters.
	/// @param[out] found_inode Receives the resolved entry inode on success.
	/// @param[out] attr Receives attributes for the resolved entry on success (from the last
	///                  component lookup or root SPECIAL_INODE_ROOT handling).
	///
	/// @return SAUNAFS_STATUS_OK on success, or an error code propagated from `lookup()`.
	///
	/// @note If the resolved path points to the root inode, this function calls getAttr
	///       to properly fetch the root's attributes.
	virtual uint8_t wholePathLookup(const FsContext &context, inode_t parent,
	                                const std::string &path, inode_t *found_inode,
	                                Attributes &attr) = 0;

	/// Retrieves the attributes of a filesystem node.
	///
	/// Verifies the session (read-only, non-meta), resolves the inode with no read/write/execute
	/// permission requirement, and fills @p attr from the node and the caller's session
	/// credentials. Does not write to the changelog.
	///
	/// @param context The FS operation context (user credentials, session flags, timestamp).
	/// @param fsOpContext The operation context carrying a read-only transaction in KV backends.
	/// @param inode The inode whose attributes to read.
	/// @param[out] attr Receives the node's attributes; zero-filled on any error return.
	/// @return SAUNAFS_STATUS_OK on success, or a session/visibility error from
	///         verifySession/getNodeForOperation (e.g. SAUNAFS_ERROR_EPERM, SAUNAFS_ERROR_ENOENT).
	virtual uint8_t getAttr(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                        inode_t inode, Attributes &attr) = 0;

	/// Attempts to initiate a file truncate operation (phase 1).
	///
	/// This method is the first phase of a file truncate operation. The complete workflow
	/// depends on whether asynchronous chunk operations are required:
	/// - Simple path: trySetLength() → doSetLength() (2 phases)
	/// - Async path: trySetLength() → [async chunk work] → endSetLength() → doSetLength() (3
	/// phases)
	///
	/// This function checks whether the truncate can proceed immediately or requires
	/// asynchronous chunk operations on chunkservers. If the new length is not chunk-aligned
	/// and a chunk exists at the truncation point, the chunk must be truncated, which is an
	/// asynchronous operation handled by chunkservers.
	///
	/// The function performs the following validations and operations:
	/// - Session verification (must be read-write, non-meta session)
	/// - Node verification (must be a file and exist)
	/// - Permission checks (write permission if file not opened, otherwise just existence)
	/// - Chunk truncation initiation if the new length falls within an existing chunk
	///
	/// Chunk truncation workflow:
	/// - If the new length is not chunk-aligned (length & SFSCHUNKMASK != 0), the function
	///   checks if a chunk exists at the truncation index
	/// - If a chunk exists, it initiates an asynchronous chunk truncation via chunk_multi_truncate
	/// - The chunk operation is logged to the changelog (TRUNC entry)
	/// - Returns SAUNAFS_ERROR_DELAYED to indicate the operation is pending
	///
	/// The denyTruncatingParity parameter prevents truncation of parity chunks (used for
	/// erasure-coded files) when truncating down. This is necessary because parity chunks
	/// cannot be partially truncated.
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param inode The inode number of the file to truncate.
	/// @param opened Non-zero if the file is currently opened (affects permission checking:
	///               if 0, write permission is required; if non-zero, only existence is checked).
	/// @param length The new target length for the file in bytes.
	/// @param denyTruncatingParity If true, prevents truncation of parity chunks when truncating
	///                             down (used for erasure-coded files).
	/// @param lockid Lock identifier for the chunk operation (used to prevent concurrent
	///               modifications).
	/// @param[out] attr Attributes structure to be filled with the file's current attributes
	///                  (only filled if operation completes immediately).
	/// @param[out] chunkid Pointer to receive the chunk ID if chunk truncation is initiated
	///                     (used to track the async operation).
	///
	/// @return SAUNAFS_STATUS_OK if the operation can proceed immediately (no chunk work needed),
	///         SAUNAFS_ERROR_DELAYED if chunk truncation was initiated (operation pending),
	///         or one of the following error codes:
	///         - SAUNAFS_ERROR_EPERM if session permissions are insufficient
	///         - SAUNAFS_ERROR_EACCES if write access is denied
	///         - SAUNAFS_ERROR_ENOENT if the file doesn't exist
	///         - SAUNAFS_ERROR_NOTPOSSIBLE if truncation of parity chunks is denied
	///         - Other error codes from chunk operations
	///
	/// @note If this function returns SAUNAFS_STATUS_OK, call doSetLength() immediately.
	/// @note If this function returns SAUNAFS_ERROR_DELAYED, call endSetLength() after the
	///       async chunk operation completes, then call doSetLength() to finalize.
	/// @note The actual file length metadata is not modified by this function; only doSetLength()
	///       modifies the file length and removes chunks beyond the new length.
	/// @see endSetLength, doSetLength
	virtual uint8_t trySetLength(const FsContext &context,
	                             const FilesystemOperationContext &fsOpContext, inode_t inode,
	                             uint8_t opened, uint64_t length, bool denyTruncatingParity,
	                             uint32_t lockid, Attributes &attr, uint64_t *chunkid) = 0;

	/// Finalizes a file truncate operation by setting the file length metadata (phase 2).
	///
	/// This method is the final phase of a file truncate operation. It updates the file's
	/// length metadata, removes all chunks beyond the new length, updates timestamps, and
	/// logs the operation to the changelog. This function always erases chunks beyond the
	/// new length because it's only called when finalizing a truncate operation.
	///
	/// This function is called in two scenarios:
	/// 1. Immediately after trySetLength() returns SAUNAFS_STATUS_OK (no async work needed)
	/// 2. After endSetLength() unlocks the chunk (when trySetLength returned DELAYED and
	///    the async chunk operation completed)
	///
	/// The function performs the following operations:
	/// - Session verification (must be read-write session)
	/// - Node verification (must be a file and exist)
	/// - Sets the file's length to the specified value
	/// - Removes all chunks beyond the new length (eraseFurtherChunks = true)
	/// - Updates the file's modification time (mtime) to current timestamp
	/// - Updates the file's change time (ctime) to current timestamp
	/// - Logs the LENGTH operation to the changelog for replication
	/// - Updates filesystem statistics
	/// - Returns updated file attributes
	///
	/// Changelog entry:
	/// - Format: LENGTH(inode, length, eraseFurtherChunks)
	/// - The eraseFurtherChunks flag is always 1 (true) for doSetLength
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param inode The inode number of the file to set the length for.
	/// @param length The new length for the file in bytes.
	/// @param[out] attr Attributes structure to be filled with the file's updated attributes
	///                  after the length change.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - SAUNAFS_ERROR_EPERM if session permissions are insufficient
	///         - SAUNAFS_ERROR_ENOENT if the file doesn't exist
	///         - SAUNAFS_ERROR_EINVAL if the node is not a file
	///         - Other error codes from node operations
	///
	/// @note This function must only be called after trySetLength() completes successfully
	///       (and after endSetLength() if the async path was taken).
	/// @note The function always removes chunks beyond the new length (eraseFurtherChunks=true).
	/// @see trySetLength, endSetLength
	virtual uint8_t doSetLength(const FsContext &context,
	                            const FilesystemOperationContext &fsOpContext, inode_t inode,
	                            uint64_t length, Attributes &attr) = 0;

	/// Unlocks a chunk after a truncate operation completes (phase 1.5).
	///
	/// This method is called to release the chunk lock acquired during a truncate operation
	/// initiated by trySetLength(). It's an intermediate cleanup step in the truncate workflow
	/// that occurs after the asynchronous chunk operation completes on the chunkservers but
	/// before the final doSetLength() call that updates the file metadata.
	///
	/// The complete truncate workflow is:
	/// 1. trySetLength() - Initiates truncate, may lock a chunk for truncation (returns DELAYED)
	/// 2. [Asynchronous chunk truncation on chunkservers]
	/// 3. endSetLength() - Unlocks the chunk (this function)
	/// 4. doSetLength() - Finalizes by updating file length metadata
	///
	/// The function performs the following operations:
	/// - Logs an UNLOCK operation to the changelog for replication
	/// - Unlocks the specified chunk via chunk_unlock()
	/// - Updates filesystem checksum
	///
	/// Changelog entry:
	/// - Format: UNLOCK(chunkid)
	///
	/// This function is called in two scenarios:
	/// 1. After a FUSE_TRUNCATE or FUSE_TRUNCATE_END delayed operation completes
	/// 2. When cleaning up after a failed truncate operation
	///
	/// @param chunkid The chunk ID to unlock (obtained from trySetLength's chunkid output).
	/// @param fsOpContext The filesystem operation context (transaction).
	///
	/// @return SAUNAFS_STATUS_OK on success, or SAUNAFS_ERROR_NOCHUNK if the chunk doesn't exist.
	///
	/// @note This function does not modify file metadata; it only unlocks the chunk.
	/// @note The chunk lock is maintained between trySetLength and endSetLength to prevent
	///       concurrent modifications during the asynchronous chunk operation.
	/// @note After calling this function, doSetLength() should be called to complete the truncate.
	/// @note This function must be called for any truncate flow in which trySetLength()
	///       acquired a chunk lock, including both delayed chunkserver truncation
	///       (SAUNAFS_ERROR_DELAYED) and client-performs truncate/end flows where
	///       trySetLength() can return SAUNAFS_ERROR_NOTPOSSIBLE (e.g. FUSE_TRUNCATE_END).
	/// @see trySetLength, doSetLength
	virtual uint8_t endSetLength(const FilesystemOperationContext &fsOpContext,
	                             uint64_t chunkid) = 0;

	/// Sets attributes for a filesystem node (file, directory, etc.).
	///
	/// This method modifies various attributes of a filesystem node, including permissions (mode),
	/// ownership (uid/gid), and timestamps (atime/mtime). It performs comprehensive validation
	/// including session verification, permission checks, and quota enforcement. The operation
	/// supports selective attribute modification through the setmask parameter, allowing
	/// operations like chmod, chown, and touch to be performed individually or in combination.
	///
	/// Key features and validations:
	/// - Session verification (must be read-write, non-meta session)
	/// - Node existence and accessibility verification
	/// - Permission checks based on user privileges and ownership
	/// - SUID/SGID bit clearing based on sugidclearmode policy
	/// - Timestamp updates with proper access time handling
	///
	/// The setmask parameter controls which attributes are modified:
	/// - SET_MODE_FLAG: Update file permissions (mode)
	/// - SET_UID_FLAG: Change user ownership
	/// - SET_GID_FLAG: Change group ownership
	/// - SET_ATIME_FLAG/SET_ATIME_NOW_FLAG: Update access time
	/// - SET_MTIME_FLAG/SET_MTIME_NOW_FLAG: Update modification time
	///
	/// SUID/SGID clearing follows different policies based on sugidclearmode:
	/// - kAlways: Always clear both bits on chown
	/// - kOsx: Clear on chown by non-root users
	/// - kBsd: Clear on gid change by non-root users
	/// - kExt: Clear based on group execute permission
	/// - kSfs: Extended policy for directories and files
	/// - kNever: Never clear (not recommended for security)
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param inode The inode number of the node whose attributes to modify.
	/// @param setmask Bitmask indicating which attributes to set (SET_MODE_FLAG, SET_UID_FLAG,
	/// etc.).
	/// @param attrmode New file mode/permissions (used if SET_MODE_FLAG is set).
	/// @param attruid New user ID (used if SET_UID_FLAG is set).
	/// @param attrgid New group ID (used if SET_GID_FLAG is set).
	/// @param attratime New access time (used if SET_ATIME_FLAG is set).
	/// @param attrmtime New modification time (used if SET_MTIME_FLAG is set).
	/// @param sugidclearmode Policy for clearing SUID/SGID bits on ownership changes.
	/// @param[out] attr Attributes structure to be filled with the node's updated attributes.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - SAUNAFS_ERROR_EPERM if permission denied (not owner, invalid uid/gid, etc.)
	///         - SAUNAFS_ERROR_EACCES if access denied based on current permissions
	///         - SAUNAFS_ERROR_ENOENT if node doesn't exist
	///         - Other error codes as returned by node operations
	///
	/// @note This operation updates the node's ctime (change time) to the current timestamp.
	/// @note SUID/SGID bits are cleared according to the specified sugidclearmode policy.
	/// @note For non-root users, various restrictions apply to prevent privilege escalation.
	virtual uint8_t setAttr(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                        inode_t inode, uint8_t setmask, uint16_t attrmode, uint32_t attruid,
	                        uint32_t attrgid, uint32_t attratime, uint32_t attrmtime,
	                        SugidClearMode sugidclearmode, Attributes &attr) = 0;

	/// Reads the target path stored in a symbolic link.
	///
	/// Verifies the session (read-only, non-meta), resolves `inode` with no read/write/execute
	/// permission requirement, and requires the node to be a symlink. On success, copies the target
	/// into `path` and updates atime, emitting an ACCESS changelog entry when the timestamp changes
	/// and atime updates are enabled.
	///
	/// @param context The FS operation context (user credentials, session flags, timestamp).
	/// @param fsOpContext The operation context; needs a read-write transaction to persist atime in
	///                    KV backends.
	/// @param inode The symbolic link inode to read.
	/// @param[out] path Receives the stored symlink target on success.
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_ENOENT if the session is meta-only or the inode cannot be resolved.
	/// @return SAUNAFS_ERROR_EPERM if the inode is outside the session-visible namespace.
	/// @return SAUNAFS_ERROR_EINVAL if the resolved node is not a symbolic link.
	virtual uint8_t readlink(const FsContext &context,
	                         const FilesystemOperationContext &fsOpContext, inode_t inode,
	                         std::string &path) = 0;

	/// Reports filesystem space and inode statistics visible from the session root.
	///
	/// For a full-root session, returns global trash and reserved space. For a subdirectory-root
	/// session, trash and reserved space are reported as zero and the inode count is taken from
	/// that subtree. Total and available space come from chunkserver space accounting and may be
	/// adjusted by quota rules for the resolved root. If the root cannot be resolved as a
	/// directory, `totalspace`, `availspace`, and `inodes` are reported as zero; for unresolved
	/// subdirectory roots, all output statistics are zero.
	///
	/// @param context The FS operation context containing the session root.
	/// @param fsOpContext The operation context used by transactional backends for reads.
	/// @param[out] totalspace Total data space in bytes.
	/// @param[out] availspace Available data space in bytes.
	/// @param[out] trashspace Trash space in bytes, or zero for subdirectory roots.
	/// @param[out] reservedspace Reserved-file space in bytes, or zero for subdirectory roots.
	/// @param[out] inodes Inode count for the visible root.
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

	/// Removes one chunk reference from a file-like node by chunk id.
	///
	/// Used to roll back a chunk allocation after chunkserver-side work fails. Verifies a
	/// read-write session, resolves `inode` as a writable file-like node, locates the first slot
	/// equal to `chunkId`, drops the file's reference in chunk metadata, clears that slot, and
	/// logs `REPAIR(inode,index):0`.
	///
	/// @param context The FS operation context (user credentials, session flags, timestamp).
	/// @param fsOpContext The operation context (read-write transaction in KV backends).
	/// @param inode The file-like node to update.
	/// @param chunkId The chunk id to remove. Must be non-zero.
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_NOCHUNK if `chunkId` is zero, absent from the file layout, or absent
	///         from chunk metadata.
	/// @return SAUNAFS_ERROR_EROFS if the session is read-only.
	/// @return SAUNAFS_ERROR_EACCES if write access to the file is denied.
	/// @return SAUNAFS_ERROR_EPERM if the inode is not file-like or not visible in the session.
	/// @return SAUNAFS_ERROR_ENOENT if the inode cannot be resolved.
	/// @return SAUNAFS_ERROR_CHUNKLOST if chunk metadata cannot remove the file reference cleanly.
	virtual uint8_t removeChunkFromFile(const FsContext &context,
	                                    const FilesystemOperationContext &fsOpContext,
	                                    inode_t inode, uint64_t chunkId) = 0;

	/// Scans and repairs chunk metadata for a file-like node.
	///
	/// Verifies a read-write session, resolves `inode` as a writable file-like node, and walks
	/// every chunk slot. Each changed slot is logged as `REPAIR(inode,index):version`: `version ==
	/// 0` means the file's chunk reference was erased, otherwise the slot was corrected to that
	/// version. With `correct_only` non-zero, unrecoverable missing/lost chunks are kept (counted
	/// as unchanged) instead of being erased; recoverable version corrections still apply.
	///
	/// @param context The FS operation context (user credentials, session flags, timestamp).
	/// @param inode The file-like node to repair.
	/// @param correct_only Non-zero to avoid erasing unrecoverable chunks.
	/// @param[out] notchanged Chunk slots left unchanged.
	/// @param[out] erased Chunk slots whose reference was erased from the file layout.
	/// @param[out] repaired Chunk slots corrected to a recoverable version.
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_EROFS if the session is read-only.
	/// @return SAUNAFS_ERROR_EACCES if write access to the file is denied.
	/// @return SAUNAFS_ERROR_EPERM if the inode is not file-like or not visible in the session.
	/// @return SAUNAFS_ERROR_ENOENT if the inode cannot be resolved.
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

	/// Removes a filesystem entry and all nested contents as a task-manager job.
	///
	/// Verifies a read-write, non-meta session, resolves `parent` as a writable directory,
	/// verifies that `name` exists under it, and submits a recursive remove task. The task walks
	/// directories depth-first, unlinks files and empty directories, logs each removal as
	/// `UNLINK(parent,name):inode`, and performs per-task transaction commits in transactional
	/// backends.
	///
	/// The initial batch is processed synchronously. If the operation finishes in that batch, the
	/// final status is returned directly and `callback` is not invoked. Otherwise the job remains
	/// queued, SAUNAFS_ERROR_WAITING is returned, and `callback` receives the final status.
	///
	/// @param context The FS operation context (user credentials, session flags, timestamp).
	/// @param parent The parent directory inode containing the entry to remove.
	/// @param name The entry name to remove recursively.
	/// @param callback Completion callback used only when the job continues asynchronously.
	/// @param job_id The task-manager job id to use for the submitted recursive remove job.
	/// @return SAUNAFS_STATUS_OK if removal completed during the initial batch.
	/// @return SAUNAFS_ERROR_WAITING if the job was accepted for asynchronous completion.
	/// @return SAUNAFS_ERROR_EROFS if the session is read-only.
	/// @return SAUNAFS_ERROR_ENOENT if the session is meta-only, `parent` cannot be resolved, or
	///         `name` does not exist.
	/// @return SAUNAFS_ERROR_ENOTDIR if `parent` is not a directory.
	/// @return SAUNAFS_ERROR_EACCES if write access to a processed parent directory is denied.
	/// @return SAUNAFS_ERROR_EPERM if `parent` is outside the session-visible namespace or
	///         sticky-bit checks reject a processed entry.
	/// @return SAUNAFS_ERROR_ENOTEMPTY if concurrent changes keep adding entries to a directory
	///         being removed.
	/// @return SAUNAFS_ERROR_IO if a transactional backend cannot commit a task step.
	virtual uint8_t recursiveRemove(const FsContext &context, inode_t parent, const HString &name,
	                                const std::function<void(int)> &callback, uint32_t job_id) = 0;

	/// Calculates the serialized directory listing size for readdirData().
	///
	/// This is the first phase of the two-phase directory-read path. It zeroes `*dnode` and
	/// `*dbuffsize`, verifies a read-only, non-meta session, resolves `inode` as a readable
	/// directory, stores an opaque directory handle in `*dnode`, and stores the exact serialized
	/// buffer size in `*dbuffsize`. If GETDIR_FLAG_WITHATTR is present in `flags`, the size
	/// includes full attributes for dot entries and children; otherwise it includes entry types
	/// only.
	///
	/// @param context The FS operation context (user credentials, session flags, timestamp).
	/// @param inode The directory inode to read.
	/// @param flags Directory read flags; only GETDIR_FLAG_WITHATTR affects the size.
	/// @param[out] dnode Receives an opaque directory handle for readdirData(); set to nullptr on
	///                   entry and left null on error.
	/// @param[out] dbuffsize Receives the required serialized buffer size; set to zero on entry and
	///                       left zero on error.
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_ENOENT if the session is meta-only or the inode cannot be resolved.
	/// @return SAUNAFS_ERROR_ENOTDIR if the inode is not a directory.
	/// @return SAUNAFS_ERROR_EACCES if read access to the directory is denied.
	/// @return SAUNAFS_ERROR_EPERM if the inode is outside the session-visible namespace.
	virtual uint8_t readdirSize(const FsContext &context, inode_t inode, uint8_t flags,
	                            void **dnode, uint32_t *dbuffsize) = 0;

	/// Serializes directory entries into a buffer sized by readdirSize().
	///
	/// This is the second phase of the two-phase directory-read path. The caller must pass the
	/// `dnode` returned by a successful readdirSize() call and a `dbuff` buffer of the reported
	/// size. The function updates directory atime when needed, emitting an ACCESS changelog entry
	/// when the timestamp changes and atime updates are enabled, then serializes `.` and `..` plus
	/// all child entries, including either full attributes or entry types according to
	/// GETDIR_FLAG_WITHATTR. It does not perform validation and has no status return.
	///
	/// @param context The FS operation context (user credentials, session flags, root inode).
	/// @param fsOpContext The operation context; needs a read-write transaction to persist atime in
	///                    transactional backends.
	/// @param flags Directory read flags; only GETDIR_FLAG_WITHATTR affects serialization.
	/// @param dnode Opaque directory handle returned by readdirSize().
	/// @param[out] dbuff Output buffer with at least the size returned by readdirSize().
	virtual void readdirData(const FsContext &context,
	                         const FilesystemOperationContext &fsOpContext, uint8_t flags,
	                         void *dnode, uint8_t *dbuff) = 0;

	/// Reads a paginated list of entries from a directory.
	///
	/// Retrieves directory entries starting from a specified index, with support for
	/// pagination. Verifies session validity and read permissions before accessing
	/// the directory contents. Updates the directory's access time upon successful read.
	///
	/// @param context Session context containing user credentials (uid, gid), session
	///                flags, and root inode information.
	/// @param fsOpContext The filesystem operation context (transaction).
	/// @param inode The inode number of the directory to read.
	/// @param first_entry Starting index for pagination (offset into the directory listing).
	/// @param number_of_entries Maximum number of entries to return.
	/// @param[out] dir_entries Output vector populated with directory entries, each
	///                         containing index, next_index, inode, name, and attributes.
	/// @return SAUNAFS_STATUS_OK on success, or an appropriate error code on failure
	///         (e.g., SAUNAFS_ERROR_ENOENT if directory not found,
	///         SAUNAFS_ERROR_EACCES if permission denied).
	virtual uint8_t readdir(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                        inode_t inode, uint64_t first_entry, uint64_t number_of_entries,
	                        std::vector<DirectoryEntry> &dir_entries) = 0;

	virtual uint8_t checkFile(const FsContext &context, inode_t inode,
	                          ChunkCountArray &chunkCount) = 0;
	virtual uint8_t openCheck(const FsContext &context,
	                          const FilesystemOperationContext &fsOpContext, inode_t inode,
	                          uint8_t flags, Attributes &attr) = 0;
	virtual uint8_t getGoal(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                        inode_t inode, uint8_t gmode, GoalStatistics &fgtab,
	                        GoalStatistics &dgtab) = 0;

	/// Retrieves aggregated extra-attribute statistics for a node or subtree.
	///
	/// Resolves `inode` in the caller session and aggregates counts of extra-attribute
	/// combinations into separate histograms for non-directory nodes and directory nodes.
	/// Traversal depth is controlled by `gmode` (`GMODE_NORMAL` for one node,
	/// `GMODE_RECURSIVE` for subtree).
	///
	/// Histogram semantics:
	/// - `fileEAttrTab` indexes are built from non-directory extraattr bits
	///   `EATTR_NOOWNER | EATTR_NOACACHE | EATTR_NODATACACHE`.
	/// - `dirEAttrTab` indexes are built from directory extraattr bits (full extraattr nibble).
	///
	/// @param context The FS operation context (credentials/session/timestamp).
	/// @param inode Root inode for the query.
	/// @param gmode Traversal mode (`GMODE_NORMAL` or `GMODE_RECURSIVE`).
	/// @param[out] fileEAttrTab Histogram for non-directory nodes.
	/// @param[out] dirEAttrTab Histogram for directory nodes.
	///
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_EINVAL if `gmode` is invalid.
	/// @return Session/visibility errors returned by verifySession/getNodeForOperation
	///         (for example SAUNAFS_ERROR_ENOENT, SAUNAFS_ERROR_EPERM, SAUNAFS_ERROR_EROFS).
	virtual uint8_t getExtraAttr(const FsContext &context, inode_t inode, uint8_t gmode,
	                             ExtraAttributesArray &fileEAttrTab,
	                             ExtraAttributesArray &dirEAttrTab) = 0;

	/// Lists extended attribute names for an inode.
	///
	/// Verifies the session (read-only, non-meta) and resolves the inode with
	/// read permission check (or no permission check if the file is already opened).
	/// On success, returns the total serialized size of all xattr names (including
	/// the always-present "system.richacl\0" prefix) and an owned result containing
	/// the serialized user-defined attribute names.
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The operation context carrying a transaction in KV backends.
	/// @param inode The inode to list attributes for.
	/// @param opened Non-zero if the file is already opened (skips permission check).
	/// @param[out] result Populated with the serialized xattr name data (excluding the
	///                    kAclXattrs prefix, which the caller must prepend).
	/// @param[out] xasize Total byte size of the serialized xattr name list, including
	///                    the "system.richacl\0" prefix and a trailing '\0' per name.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - Session or permission errors from verifySession/getNodeForOperation
	///         - SAUNAFS_ERROR_ERANGE if the total xattr name list exceeds SFS_XATTR_LIST_MAX
	virtual uint8_t listXAttr(const FsContext &context,
	                          const FilesystemOperationContext &fsOpContext, inode_t inode,
	                          uint8_t opened, XAttrListResult &result, uint32_t *xasize) = 0;

	/// Retrieves the value of a single extended attribute.
	///
	/// Verifies the session (read-only, non-meta), resolves the inode with read
	/// permission check (or no check if already opened), and validates the attribute
	/// name (must not contain null bytes). Then looks up the named attribute and
	/// returns an owned copy of its value.
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The operation context carrying a transaction in KV backends.
	/// @param inode The inode to retrieve the attribute from.
	/// @param opened Non-zero if the file is already opened (skips permission check).
	/// @param anleng Length of the attribute name in bytes.
	/// @param attrname The attribute name bytes (not null-terminated).
	/// @param[out] result Populated with an owned copy of the attribute value on success.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - Session or permission errors from verifySession/getNodeForOperation
	///         - SAUNAFS_ERROR_EINVAL if the attribute name contains null bytes
	///         - SAUNAFS_ERROR_ENOATTR if the attribute does not exist
	///         - SAUNAFS_ERROR_ERANGE if the stored value exceeds SFS_XATTR_SIZE_MAX
	virtual uint8_t getXAttr(const FsContext &context,
	                         const FilesystemOperationContext &fsOpContext, inode_t inode,
	                         uint8_t opened, uint8_t anleng, const uint8_t *attrname,
	                         XAttrGetResult &result) = 0;

	/// Sets, replaces, or removes an extended attribute on an inode.
	///
	/// Verifies the session (read-write, non-meta), resolves the inode with write
	/// permission check (or no check if already opened), and validates the attribute
	/// name and mode. Depending on the mode, the attribute is created, replaced, or
	/// removed. On success, updates the inode's ctime, its checksum, and writes
	/// a SETXATTR entry to the changelog.
	///
	/// @param context The FS operation context containing user credentials and session info.
	/// @param fsOpContext The operation context carrying a read-write transaction in KV backends.
	/// @param inode The inode to set/remove the attribute on.
	/// @param opened Non-zero if the file is already opened (skips permission check).
	/// @param anleng Length of the attribute name in bytes.
	/// @param attrname The attribute name bytes (not null-terminated).
	/// @param avleng Length of the attribute value in bytes. Not applied to storage for REMOVE,
	///              but still referenced in changelog logging; pass 0 for REMOVE.
	/// @param attrvalue The attribute value bytes. Not applied to storage for REMOVE,
	///                  but still referenced in changelog logging; pass a valid pointer for REMOVE.
	/// @param mode One of XATTR_SMODE_CREATE_OR_REPLACE, XATTR_SMODE_CREATE_ONLY,
	///             XATTR_SMODE_REPLACE_ONLY, or XATTR_SMODE_REMOVE.
	///
	/// @return SAUNAFS_STATUS_OK on success, or one of the following error codes:
	///         - Session or permission errors from verifySession/getNodeForOperation
	///         - SAUNAFS_ERROR_EINVAL if the attribute name is empty or contains null bytes,
	///           or mode is invalid
	///         - SAUNAFS_ERROR_EEXIST if mode is CREATE_ONLY and the attribute already exists
	///         - SAUNAFS_ERROR_ENOATTR if mode is REPLACE_ONLY or REMOVE and the attribute
	///           does not exist
	///         - SAUNAFS_ERROR_ERANGE if the value or total name list size exceeds limits
	virtual uint8_t setXAttr(const FsContext &context,
	                         const FilesystemOperationContext &fsOpContext, inode_t inode,
	                         uint8_t opened, uint8_t anleng, const uint8_t *attrname,
	                         uint32_t avleng, const uint8_t *attrvalue, uint8_t mode) = 0;

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

	/// Returns chunk ids, versions, and locations for a file chunk range.
	///
	/// Verifies the session for a read-only operation, resolves `inode` as a readable file-like
	/// node, validates `chunk_index`, clears `chunks`, and appends one result per file chunk slot
	/// starting at `chunk_index` until `chunk_count` slots are reported or EOF is reached. If
	/// `chunk_count` is zero, the range extends to EOF. Holes are returned as entries with chunk
	/// id 0, version 0, and no locations.
	///
	/// For non-empty chunk slots, implementations return the current chunk version and available
	/// part locations, using `current_ip` to prefer closer locations where topology information is
	/// available. Does not write to the changelog.
	///
	/// @param context The FS operation context (user credentials, session flags, root inode).
	/// @param current_ip Client IP used to order chunk locations by topology distance.
	/// @param inode File-like inode whose chunk layout to inspect.
	/// @param chunk_index First file chunk index to report.
	/// @param chunk_count Maximum number of chunk slots to report; 0 means until EOF.
	/// @param[out] chunks Receives chunk entries; cleared before data is appended.
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_INDEXTOOBIG if `chunk_index` exceeds the maximum chunk index.
	/// @return SAUNAFS_ERROR_ENOENT if the inode cannot be resolved.
	/// @return SAUNAFS_ERROR_EACCES if read access to the file is denied.
	/// @return SAUNAFS_ERROR_EPERM if the inode is not file-like or not visible in the session.
	/// @return SAUNAFS_ERROR_NOCHUNK if a non-zero file chunk reference has no chunk metadata.
	virtual uint8_t getChunksInfo(const FsContext &context, uint32_t current_ip, inode_t inode,
	                              uint32_t chunk_index, uint32_t chunk_count,
	                              std::vector<ChunkWithAddressAndLabel> &chunks) = 0;

	/// Aggregates trash-time counters for a node or subtree.
	///
	/// Validates `gmode`, verifies the session for a read-only operation, resolves `inode` as a
	/// file-like node or directory with no permission mode requirement, and accumulates trash-time
	/// values into `fileTrashtimes` and `dirTrashtimes`. `GMODE_NORMAL` counts only `inode`;
	/// `GMODE_RECURSIVE` descends into directory children. The maps are not cleared before
	/// aggregation.
	///
	/// @param context The FS operation context (user credentials, session flags, root inode).
	/// @param inode Root inode for the query.
	/// @param gmode Traversal mode (`GMODE_NORMAL` or `GMODE_RECURSIVE`).
	/// @param[in,out] fileTrashtimes Trash-time counters for file-like nodes.
	/// @param[in,out] dirTrashtimes Trash-time counters for directory nodes.
	/// @return SAUNAFS_STATUS_OK on success.
	/// @return SAUNAFS_ERROR_EINVAL if `gmode` is invalid.
	/// @return SAUNAFS_ERROR_ENOENT if the inode cannot be resolved.
	/// @return SAUNAFS_ERROR_EPERM if the inode is neither file-like nor a directory, or is
	///         not visible in the session.
	virtual uint8_t getTrashTimePrepare(const FsContext &context, inode_t inode, uint8_t gmode,
	                                    TrashtimeMap &fileTrashtimes,
	                                    TrashtimeMap &dirTrashtimes) = 0;

	/// Merges a POSIX ACL (kAccess or kDefault) into the node's stored RichACL.
	///
	/// Verifies the session (read-write, non-meta), resolves the inode, then delegates to
	/// IFilesystemNodeOperations::setAcl(AclType, AccessControlList). On success, writes a
	/// SETACL(inode, 'a'|'d', acl_string) entry to the changelog (master personality) or
	/// increments the metadata version (shadow personality).
	///
	/// @param context The FS operation context (user credentials, session flags, timestamp).
	/// @param fsOpContext The operation context carrying a read-write transaction in KV backends.
	/// @param inode The inode of the node on which to set the ACL.
	/// @param type Must be AclType::kAccess or AclType::kDefault.
	/// @param acl The POSIX ACL entries to merge in.
	/// @return SAUNAFS_STATUS_OK on success, or a session/permission/node-lookup error.
	virtual uint8_t setAcl(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                       inode_t inode, AclType type, const AccessControlList &acl) = 0;

	/// Stores a RichACL directly on a node, replacing any previously stored ACL.
	///
	/// Verifies the session (read-write, non-meta), resolves the inode, calls
	/// acl.toString() to produce the changelog representation, then delegates to
	/// IFilesystemNodeOperations::setAcl(RichACL). On success, writes a
	/// SETRICHACL(inode, acl_string) entry to the changelog (master personality) or
	/// increments the metadata version (shadow personality).
	///
	/// @param context The FS operation context (user credentials, session flags, timestamp).
	/// @param fsOpContext The operation context carrying a read-write transaction in KV backends.
	/// @param inode The inode of the node on which to set the ACL.
	/// @param acl The RichACL to store.
	/// @return SAUNAFS_STATUS_OK on success, or a session/permission/node-lookup error.
	virtual uint8_t setAcl(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                       inode_t inode, const RichACL &acl) = 0;

	/// Retrieves the stored RichACL for a node, given its inode.
	///
	/// Verifies the session (read-only, any session type), resolves the inode, then delegates
	/// to IFilesystemNodeOperations::getAcl. Does not write to the changelog and does not
	/// need a read-write transaction.
	///
	/// @param context The FS operation context (user credentials, session flags, timestamp).
	/// @param fsOpContext The operation context carrying a read-only transaction in KV backends.
	/// @param inode The inode of the node whose ACL to retrieve.
	/// @param[out] acl Receives the stored RichACL.
	/// @return SAUNAFS_STATUS_OK on success, SAUNAFS_ERROR_ENOATTR if no ACL is stored,
	///         or a session/permission/node-lookup error.
	virtual uint8_t getAcl(const FsContext &context, const FilesystemOperationContext &fsOpContext,
	                       inode_t inode, RichACL &acl) = 0;

	// Functions which modify metadata or return some information.
	// To be used by the master server with personality == kMaster

	/// Retrieves filesystem-wide capacity and object counters.
	///
	/// This method fills all output parameters with the current master-side values used by
	/// `CLTOMA_INFO`/`MATOCL_INFO` (`SaunaFsStatistics`):
	/// - `totalSpace` and `availableSpace` come from `matocsserv_getspace()`.
	/// - All remaining parameters reflect the current metadata counters, sourced from the
	///   backend used by the active implementation (in-memory or KV store).
	///
	/// @param[out] totalSpace Receives total chunkserver space in bytes.
	/// @param[out] availableSpace Receives currently available chunkserver space in bytes.
	/// @param[out] trashSpace Receives total size in bytes of files in trash.
	/// @param[out] trashNodes Receives number of files in trash.
	/// @param[out] reservedSpace Receives total size in bytes of files in reserved.
	/// @param[out] reservedNodes Receives number of files in reserved.
	/// @param[out] inodes Receives total number of metadata nodes.
	/// @param[out] directoryNodes Receives number of directory nodes.
	/// @param[out] fileNodes Receives number of file-type nodes.
	/// @param[out] linkNodes Receives number of symbolic-link nodes.
	virtual void getFSStats(uint64_t *totalSpace, uint64_t *availableSpace, uint64_t *trashSpace,
	                        inode_t *trashNodes, uint64_t *reservedSpace, inode_t *reservedNodes,
	                        inode_t *inodes, inode_t *directoryNodes, inode_t *fileNodes,
	                        inode_t *linkNodes) = 0;

	/// Returns the byte length of the directory-path representation for `inode`.
	///
	/// Return value:
	/// - If the inode is not found: `kDirPathNotFound.size()`.
	/// - If the node is not a directory: `kDirPathNotDirectory.size()`.
	/// - If the node is a directory: full absolute path length in bytes, including leading `/`.
	///
	/// Use this value to pre-allocate the buffer passed to `getDirPathData()` for an
	/// untruncated result.
	///
	/// @param fsOpContext The operation context (used for node lookups in KV backends).
	/// @param inode The inode of the directory node to measure.
	/// @return The number of bytes required for `getDirPathData()` to produce an
	///         untruncated result for this inode.
	virtual uint32_t getDirPathSize(const FilesystemOperationContext &fsOpContext,
	                                inode_t inode) = 0;

	/// Writes the directory-path representation for `inode` into a caller-supplied buffer.
	///
	/// Resolves `inode` to a node and writes into `buff` (up to `size` bytes):
	/// - If the inode is not found and `size >= kDirPathNotFound.size()`: writes
	///   `kDirPathNotFound` into `buff`; writes nothing if `size` is smaller.
	/// - If the node is not a directory and `size >= kDirPathNotDirectory.size()`: writes
	///   `kDirPathNotDirectory` into `buff`; writes nothing if `size` is smaller.
	/// - If the node is a valid directory:
	///   - If `size == 0`, nothing is written.
	///   - If `size > 0`, `buff[0]` is always `'/'` and the remaining bytes are filled with path
	///     data. If the buffer is too small for the full path, output is truncated: `buff[0]`
	///     remains `'/'` and the remaining bytes contain a right-aligned tail of the path.
	///
	/// The buffer is not null-terminated. Callers should use `getDirPathSize()` first to
	/// obtain the exact buffer size needed for an untruncated result.
	///
	/// @param fsOpContext The operation context (used for node lookups in KV backends).
	/// @param inode The inode of the directory node whose path to write.
	/// @param buff Pointer to the output buffer.
	/// @param size The size of the output buffer in bytes.
	virtual void getDirPathData(const FilesystemOperationContext &fsOpContext, inode_t inode,
	                            uint8_t *buff, uint32_t size) = 0;

	/// Resolves a filesystem path to a directory inode, starting from the root.
	///
	/// Walks each `/`-separated component of `path` from the filesystem root directory.
	/// Multiple consecutive `/` characters are treated as a single separator.
	/// An empty path or a path consisting only of `/` characters resolves to the root inode.
	/// On success, writes the inode of the resolved directory to `*rootinode`.
	///
	/// @note Primarily used during FUSE session registration to resolve the mount subpath
	///       into the inode that becomes the session's visible filesystem root.
	///
	/// @param[out] rootinode Receives the inode of the directory at `path`.
	/// @param path Null-terminated path to resolve (e.g. `"subvol/dir"`).
	/// @return `SAUNAFS_STATUS_OK` on success.
	/// @return `SAUNAFS_ERROR_ENOENT` if any path component is not found.
	/// @return `SAUNAFS_ERROR_ENOTDIR` if any resolved path component is not a directory.
	/// @return `SAUNAFS_ERROR_EINVAL` if a path component name is invalid.
	virtual uint8_t getRootInode(inode_t *rootinode, const uint8_t *path) = 0;

	virtual uint8_t readChunk(const FilesystemOperationContext &fsOpContext, inode_t inode,
	                          uint32_t indx, uint64_t *chunkid, uint64_t *length) = 0;
	virtual uint8_t writeEnd(const FilesystemOperationContext &fsOpContext, inode_t inode,
	                         uint64_t length, uint64_t chunkid, uint32_t lockid) = 0;
	virtual void getTrashTimeStore(TrashtimeMap &fileTrashtimes, TrashtimeMap &dirTrashtimes,
	                               uint8_t *buff) = 0;

	virtual uint32_t newSessionId() = 0;

	// RESERVED
	virtual uint8_t readReservedSize(inode_t rootinode, uint8_t sesflags, uint32_t *dbuffsize) = 0;
	virtual void readReservedData(inode_t rootinode, uint8_t sesflags, uint8_t *dbuff) = 0;
	virtual void readReserved(uint32_t off, uint32_t max_entries,
	                          std::vector<NamedInodeEntry> &entries) = 0;
	virtual void readReserved(const FilesystemOperationContext &fsOpContext, uint64_t handleOffset,
	                          uint32_t maxEntries, std::vector<HandleInodeEntry> &entries) = 0;

	// TRASH
	virtual uint8_t readTrashSize(inode_t rootinode, uint8_t sesflags, uint32_t *dbuffsize) = 0;
	virtual void readTrashData(inode_t rootinode, uint8_t sesflags, uint8_t *dbuff) = 0;
	virtual void readTrash(uint32_t off, uint32_t max_entries,
	                       std::vector<NamedInodeEntry> &entries) = 0;
	virtual void readTrash(const FilesystemOperationContext &fsOpContext, uint64_t handleOffset,
	                       uint32_t maxEntries, std::vector<HandleInodeEntry> &entries) = 0;
	virtual uint8_t getTrashPath(const FilesystemOperationContext &fsOpContext, inode_t rootinode,
	                             uint8_t sesflags, inode_t inode, std::string &path) = 0;

	// RESERVED+TRASH
	virtual uint8_t getDetachedAttr(const FilesystemOperationContext &fsOpContext,
	                                inode_t rootinode, uint8_t sesflags, inode_t inode,
	                                Attributes &attr, uint8_t dtype) = 0;

	// EXTRA
	virtual uint8_t getDirStats(const FsContext &context, inode_t inode, inode_t *inodes,
	                            inode_t *dirs, inode_t *files, inode_t *links, uint32_t *chunks,
	                            uint64_t *length, uint64_t *size, uint64_t *rsize) = 0;
	virtual uint8_t getChunkId(const FsContext &context,
	                           const FilesystemOperationContext &fsOpContext, inode_t inode,
	                           uint32_t index, uint64_t *chunkid) = 0;

	// SPECIAL - LOG EMERGENCY INCREASE VERSION FROM CHUNKS-MODULE
	virtual void increaseChunkVersion(const FilesystemOperationContext &fsOpContext,
	                                  uint64_t chunkid) = 0;

	virtual uint8_t fullPathByInode(const FsContext &context, inode_t inode,
	                                std::string &fullPath) = 0;
	virtual std::string fullPathByInode(const FilesystemOperationContext &fsOpContext,
	                                    inode_t initialInode) = 0;

#endif

	// QUOTAS

	/// Checks whether applying @p resourceList would exceed hard user/group limits.
	///
	/// In the in-memory implementation, only hard limits are enforced; soft limits are stored and
	/// reported but do not reject operations. A limit value of 0 means "unlimited".
	///
	/// @param fsOpContext Filesystem operation context carrying backend transaction state.
	/// @param uid User owner id used for user quota checks.
	/// @param gid Group owner id used for group quota checks.
	/// @param resourceList Resource deltas to validate (typically inodes and/or size).
	/// @return status plus whether either user or group hard quota would be exceeded.
	virtual QuotaCheckResult quotaExceededUg(
	    const FilesystemOperationContext &fsOpContext, uint32_t uid, uint32_t gid,
	    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) = 0;

	/// Checks whether applying @p resourceList would exceed hard directory-owner limits.
	///
	/// The check is performed for the node's directory owner (when applicable) and ancestor
	/// directories according to the quota traversal rules used by rename/create/write paths.
	///
	/// @param fsOpContext Filesystem operation context carrying backend transaction state.
	/// @param node Node whose directory context is validated.
	/// @param resourceList Resource deltas to validate (typically inodes and/or size).
	/// @return status plus whether any checked directory hard quota would be exceeded.
	virtual QuotaCheckResult quotaExceededDir(
	    const FilesystemOperationContext &fsOpContext, FSNode *node,
	    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) = 0;

	/// Checks destination-side directory hard limits for move/rename operations.
	///
	/// Implementations compare destination and previous ancestry and stop checks at the common
	/// ancestor, matching existing in-memory rename quota semantics.
	///
	/// @param fsOpContext Filesystem operation context carrying backend transaction state.
	/// @param node Destination directory.
	/// @param prevNode Previous/source directory.
	/// @param resourceList Resource deltas to validate for the move.
	/// @return status plus whether destination-side hard quotas would be exceeded.
	virtual QuotaCheckResult quotaExceededDirMove(
	    const FilesystemOperationContext &fsOpContext, FSNodeDirectory *node,
	    FSNodeDirectory *prevNode,
	    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) = 0;

	/// Checks both user/group and directory hard quotas for @p node.
	///
	/// This is the combined helper used by operations that need both checks in one call.
	///
	/// @param fsOpContext Filesystem operation context carrying backend transaction state.
	/// @param node Node whose owner and directory quotas are validated.
	/// @param resourceList Resource deltas to validate.
	/// @return status plus whether any relevant hard quota would be exceeded.
	virtual QuotaCheckResult quotaExceeded(
	    const FilesystemOperationContext &fsOpContext, FSNode *node,
	    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) = 0;

	/// Applies quota usage deltas after a successful metadata mutation.
	///
	/// In the in-memory implementation, this updates only user/group `used` counters
	/// incrementally. Inode-owner `used` values are derived from directory stats for reads.
	///
	/// @param fsOpContext Filesystem operation context carrying backend transaction state.
	/// @param node Mutated node whose owner usage must be updated.
	/// @param resourceList Resource deltas to apply.
	virtual void quotaUpdate(
	    const FilesystemOperationContext &fsOpContext, FSNode *node,
	    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) = 0;

	/// Removes all quota tuples for a single owner.
	///
	/// Used by inode-owner cleanup paths when an inode is removed.
	///
	/// @param fsOpContext Filesystem operation context carrying backend transaction state.
	/// @param ownerType Owner namespace (user/group/inode).
	/// @param ownerId Owner identifier.
	virtual void quotaRemove(const FilesystemOperationContext &fsOpContext,
	                         QuotaOwnerType ownerType, inode_t ownerId) = 0;

#ifndef METARESTORE
	/// Returns all quota entries visible to @p context.
	///
	/// Non-privileged sessions require quota-management permission flags. Inode-owner visibility is
	/// filtered by session root and inode `used` is reported using directory stats semantics.
	/// In the in-memory implementation this returns only resources with at least one non-zero
	/// soft/hard limit, plus the matching `used` entry for each returned resource.
	///
	/// @param context Session context used for permission and visibility checks.
	/// @param[out] results Collected quota entries.
	/// @return SAUNAFS_STATUS_OK on success or a permission error.
	virtual uint8_t quotaGetAll(const FsContext &context, std::vector<QuotaEntry> &results) = 0;

	/// Returns quota entries for selected owners.
	///
	/// Owner access is validated per owner type (user/group/inode). For each existing owner in the
	/// in-memory implementation, all rigor/resource tuples are returned (including zero-valued
	/// ones), except inode-owner `used`, which is derived from current directory stats.
	///
	/// @param context Session context used for permission and visibility checks.
	/// @param owners Owners to query.
	/// @param[out] results Collected quota entries.
	/// @return SAUNAFS_STATUS_OK on success or an access/validation error.
	virtual uint8_t quotaGet(const FsContext &context, const std::vector<QuotaOwner> &owners,
	                         std::vector<QuotaEntry> &results) = 0;

	/// Sets quota entries and writes corresponding SETQUOTA changelog records.
	///
	/// The caller provides a read-write operation context so backend-specific quota writes can be
	/// applied within the same operation scope.
	///
	/// @param context Session context used for permission checks.
	/// @param fsOpContext Filesystem operation context carrying a read-write transaction.
	/// @param entries Quota entries to set.
	/// @return SAUNAFS_STATUS_OK on success or an access/validation/storage error.
	virtual uint8_t quotaSet(const FsContext &context,
	                         const FilesystemOperationContext &fsOpContext,
	                         const std::vector<QuotaEntry> &entries) = 0;

	/// Returns display information strings for quota entries.
	///
	/// For inode owners this is a resolved path when the inode exists; for other owner types it is
	/// an empty string.
	///
	/// @param context Session context used for path resolution scope.
	/// @param entries Quota entries for which to generate info.
	/// @param[out] result Display strings aligned with @p entries.
	/// @return SAUNAFS_STATUS_OK on success.
	virtual uint8_t quotaGetInfo(const FsContext &context, const std::vector<QuotaEntry> &entries,
	                             std::vector<std::string> &result) = 0;

	/// Purges expired trash entries whose expiry timestamp is before @p timeStamp.
	///
	/// Default implementation iterates gMetadata->trash. KV backends override this to scan
	/// TRSH_TIME_ entries directly from FDB.
	///
	/// @param timeStamp Current wall-clock time (seconds since epoch). Entries with
	///                  expiryTs < timeStamp are eligible for purge.
	virtual void doEmptyTrash(uint32_t timeStamp) = 0;

	/// Forcefully releases every reserved file from each of its owning sessions, regardless
	/// of whether those sessions are still active.
	///
	/// Disabled by default (EMPTY_RESERVED_FILES_PERIOD_MSECONDS = 0). When enabled it
	/// can disrupt clients that still hold reserved files they have not yet released.
	///
	/// Default implementation iterates the in-memory reserved map. KV backends override
	/// this to scan reserved entries directly from FDB.
	///
	/// @param timeStamp Current wall-clock time (seconds since epoch).
	virtual void doEmptyReserved(uint32_t timeStamp) = 0;

	// CHECKSUM

	/// Starts recalculating metadata checksum in background.
	/// @return SAUNAFS_STATUS_OK if dump started successfully, otherwise cause of the failure.
	virtual uint8_t startChecksumRecalculation() = 0;

	/// Runs the once-per-second file-test scheduler tick.
	///
	/// The in-memory implementation preserves the node-hash scan timing and counters.
	/// KV implementations may acquire backend-specific scanner ownership and publish their
	/// result state elsewhere.
	///
	/// @param timeStamp Event-loop timestamp for this scheduler tick.
	virtual void fsTestPeriodicTick(uint32_t timeStamp) = 0;

	/// Runs one background file-test step from the event loop.
	///
	/// The in-memory implementation preserves the watchdog/yield behavior. KV implementations may
	/// use backend-specific page transactions and cooperative leases.
	virtual void fsTestBackgroundStep() = 0;

	/// Returns the last published file-test report.
	///
	/// The in-memory implementation builds the report from gDefectiveNodes. KV implementations may
	/// return a previously rendered report from shared backend storage.
	///
	/// @param[out] out File-test report populated from the latest published scan results.
	virtual void fsTestGetData(FsTestReport &out) = 0;

	/// Returns a paginated list of defective files matching @p requestedFlags.
	///
	/// The cursor is opaque to callers; pass 0 to start from the beginning. The in-memory
	/// implementation keeps the hash-map position semantics; KV implementations use it
	/// as an inode-ordered scan position into FILETEST_DEFECTIVE_ rows.
	///
	/// @param requestedFlags NodeErrorFlag bits used to filter defective files.
	/// @param maxEntries Maximum number of entries to return.
	/// @param[in,out] cursor Opaque scan cursor; set to 0 to start a new scan.
	/// @return Defective file entries matching @p requestedFlags.
	virtual std::vector<DefectiveFileInfo> fsTestGetDefectiveNodes(uint8_t requestedFlags,
	                                                               uint64_t maxEntries,
	                                                               uint64_t &cursor) = 0;

	/// Removes backend-specific file-test state for a node that is being deleted.
	///
	/// KV implementations may use @p fsOpContext to clear state in the same transaction as the
	/// node removal. The in-memory implementation only erases from gDefectiveNodes.
	///
	/// @param fsOpContext Filesystem operation context carrying backend transaction state.
	/// @param inode Removed inode whose file-test state must be cleared.
	virtual void fsTestOnNodeRemoved(const FilesystemOperationContext &fsOpContext,
	                                 inode_t inode) = 0;

	/// Runs one background metadata-checksum recalculation step.
	///
	/// The in-memory implementation preserves the checksum updater behavior. KV implementations may
	/// no-op because metadata checksums are not meaningful for native KV metadata.
	virtual void backgroundChecksumStep() = 0;

	/// Reads backend-specific options from configuration.
	///
	/// Called from `fs_read_periodic_config_file()` after the master-side options are loaded,
	/// so backends can read their own keys without the master code referencing them.
	/// Implementations with no backend-specific periodic options should override as a no-op.
	virtual void readBackendPeriodicConfig() = 0;
#endif
	virtual void addFilesToChunks(bool isMetadataLoading = true) = 0;

	// Functions which apply changes from changelog, only for shadow master and metarestore
	virtual uint8_t applyChecksum(const std::string &version, uint64_t checksum) = 0;
	virtual uint8_t applyCreate(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                            inode_t parent, const HString &name, FSNodeType type, uint32_t mode,
	                            uint32_t uid, uint32_t gid, uint32_t rdev, inode_t inode) = 0;
	virtual uint8_t applyAccess(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                            inode_t inode) = 0;
	virtual uint8_t applyAttr(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                          inode_t inode, uint32_t mode, uint32_t uid, uint32_t gid,
	                          uint32_t atime, uint32_t mtime) = 0;
	virtual uint8_t applySession(uint32_t sessionid) = 0;
	virtual uint8_t applyIncreaseChunkVersion(const FilesystemOperationContext &fsOpContext,
	                                          uint64_t chunkid) = 0;
	virtual uint8_t applyLength(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                            inode_t inode, uint64_t length, bool eraseFurtherChunks) = 0;
	virtual uint8_t applyRepair(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                            inode_t inode, uint32_t indx, uint32_t nversion) = 0;
	virtual uint8_t applySetXAttr(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                              inode_t inode, uint32_t anleng, const uint8_t *attrname,
	                              uint32_t avleng, const uint8_t *attrvalue, uint32_t mode) = 0;

	/// Replays a SETACL changelog entry during metadata restore.
	///
	/// Parses @p aclString via AccessControlList::fromString, resolves the inode, decodes
	/// @p aclType ('d' -> kDefault, 'a' -> kAccess) via decodeChar, then calls
	/// IFilesystemNodeOperations::setAcl(AclType, AccessControlList). On success, increments
	/// the metadata version.
	///
	/// @param fsOpContext The operation context carrying a read-write transaction in KV backends.
	/// @param timestamp The timestamp from the changelog entry, used to update the node's ctime.
	/// @param inode The inode of the node on which to set the ACL.
	/// @param aclType Single character encoding the ACL type: 'd' for kDefault, 'a' for kAccess.
	/// @param aclString The serialized POSIX ACL string as written by AccessControlList::toString.
	/// @return SAUNAFS_STATUS_OK on success, SAUNAFS_ERROR_EINVAL if parsing or type decoding
	///         fails, SAUNAFS_ERROR_ENOENT if the inode does not exist.
	virtual uint8_t applySetAcl(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                            inode_t inode, char aclType, const char *aclString) = 0;

	/// Replays a SETRICHACL changelog entry during metadata restore.
	///
	/// Parses @p acl_string via RichACL::fromString, resolves the inode, then calls
	/// IFilesystemNodeOperations::setAcl(RichACL). On success, increments the metadata version.
	///
	/// @param fsOpContext The operation context carrying a read-write transaction in KV backends.
	/// @param timestamp The timestamp from the changelog entry, used to update the node's ctime.
	/// @param inode The inode of the node on which to set the ACL.
	/// @param acl_string The serialized RichACL string as written by RichACL::toString.
	/// @return SAUNAFS_STATUS_OK on success, SAUNAFS_ERROR_EINVAL if parsing fails,
	///         SAUNAFS_ERROR_ENOENT if the inode does not exist.
	virtual uint8_t applySetRichAcl(const FilesystemOperationContext &fsOpContext,
	                                uint32_t timestamp, inode_t inode,
	                                const std::string &acl_string) = 0;

	virtual uint8_t applyUnlink(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                            inode_t parent, const HString &name, inode_t inode) = 0;
	virtual uint8_t applyUnlock(uint64_t chunkid) = 0;
	virtual uint8_t applyTrunc(const FilesystemOperationContext &fsOpContext, uint32_t timestamp,
	                           inode_t inode, uint32_t indx, uint64_t chunkid, uint32_t lockid) = 0;

	/// Replays a SETQUOTA changelog entry during metadata restore.
	///
	/// Decodes single-character tuple selectors (rigor/resource/ownerType), applies one quota tuple
	/// update, and advances metadata version on success.
	///
	/// @param fsOpContext The operation context carrying a read-write transaction in KV backends.
	/// @param rigor Quota rigor selector ('S' or 'H').
	/// @param resource Quota resource selector ('S' for size, 'I' for inodes).
	/// @param ownerType Quota owner selector ('U', 'G', or 'I').
	/// @param ownerId Owner identifier.
	/// @param limit New tuple value.
	/// @return SAUNAFS_STATUS_OK on success, SAUNAFS_ERROR_EINVAL on decode/validation errors.
	virtual uint8_t applySetQuota(const FilesystemOperationContext &fsOpContext, char rigor,
	                              char resource, char ownerType, inode_t ownerId,
	                              uint64_t limit) = 0;

	// CHECKSUM

	/// Returns checksum of the loaded metadata.
	virtual uint64_t metadataChecksum(ChecksumMode mode) = 0;

	/// Returns whether this backend computes meaningful Master-Shadow metadata checksums.
	virtual bool metadataChecksumSupported() const = 0;

	// Lock operations

	/// Perform a flock-style (whole-file) advisory lock operation.
	///
	/// Supports placing and removing whole-file shared/exclusive advisory locks, removing pending
	/// requests and handling interruptible requests.
	/// @param context Filesystem context.
	/// @param fsOpContext The operation context carrying a transaction in KV backends.
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
	virtual int flockOperation(const FsContext &context,
	                           const FilesystemOperationContext &fsOpContext, inode_t inode,
	                           uint64_t owner, uint32_t sessionid, uint32_t reqid, uint32_t msgid,
	                           uint16_t oper, bool nonblocking,
	                           std::vector<FileLocks::Owner> &applied) = 0;

	/// Perform a POSIX byte-range (fcntl) lock operation on the filesystem.
	///
	/// Handles POSIX (fcntl) style byte-range locks: place shared/exclusive locks,
	/// release ranges, enqueue pending requests and handle interruptible requests.
	/// @param context Filesystem context (permissions, timestamp, personality, ...).
	/// @param fsOpContext The operation context carrying a transaction in KV backends.
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
	virtual int posixLockOperation(const FsContext &context,
	                               const FilesystemOperationContext &fsOpContext, inode_t inode,
	                               uint64_t start, uint64_t end, uint64_t owner, uint32_t sessionid,
	                               uint32_t reqid, uint32_t msgid, uint16_t oper, bool nonblocking,
	                               std::vector<FileLocks::Owner> &applied) = 0;

	/// Perform a POSIX lock probe on filesystem.
	/// A POSIX probe checks whether a lock request (shared/exclusive) would be blocked
	/// by existing locks without placing a lock.
	/// @param context Filesystem context.
	/// @param fsOpContext The operation context carrying a transaction in KV backends.
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
	virtual int posixLockProbe(const FsContext &context,
	                           const FilesystemOperationContext &fsOpContext, inode_t inode,
	                           uint64_t start, uint64_t end, uint64_t owner, uint32_t sessionid,
	                           uint32_t reqid, uint32_t msgid, uint16_t oper,
	                           safs_locks::FlockWrapper &info) = 0;

	/// Release (unlock + unqueue) all locks from a given session.
	/// @param context Filesystem context.
	/// @param fsOpContext The operation context carrying a transaction in KV backends.
	/// @param type Type of locks to clear (kFlock, kPosix).
	/// @param inode inode number on which to clear locks.
	/// @param sessionid Session id whose locks are to be cleared.
	/// @param applied Vector to be filled with the owners of the cleared locks.
	virtual int locksClearSession(const FsContext &context,
	                              const FilesystemOperationContext &fsOpContext, uint8_t type,
	                              inode_t inode, uint32_t sessionid,
	                              std::vector<FileLocks::Owner> &applied) = 0;

	/// List locks in the filesystem.
	/// Fills outLocks with locks matching the type and pending parameters.
	/// @param context Filesystem context (could be ignored in some implementations).
	/// @param fsOpContext The operation context carrying a transaction in KV backends.
	/// @param type Type of locks to list (kFlock, kPosix).
	/// @param pending If true, lists pending locks, otherwise lists active locks.
	/// @param start Start index for listing.
	/// @param max Maximum number of locks to list.
	/// @param outLocks Vector to be filled with the listed locks.
	virtual int locksListAll(const FsContext &context,
	                         const FilesystemOperationContext &fsOpContext, uint8_t type,
	                         bool pending, uint64_t start, uint64_t max,
	                         std::vector<safs_locks::Info> &outLocks) = 0;

	/// List locks for a specific inode.
	/// @param context Filesystem context (could be ignored in some implementations).
	/// @param fsOpContext The operation context carrying a transaction in KV backends.
	/// @param type Type of locks to list (kFlock, kPosix).
	/// @param pending If true, lists pending locks, otherwise lists active locks.
	/// @param inode inode number on which to list locks.
	/// @param start Start index for listing.
	/// @param max Maximum number of locks to list.
	/// @param outLocks Vector to be filled with the listed locks.
	virtual int locksListInode(const FsContext &context,
	                           const FilesystemOperationContext &fsOpContext, uint8_t type,
	                           bool pending, inode_t inode, uint64_t start, uint64_t max,
	                           std::vector<safs_locks::Info> &outLocks) = 0;

	/// Unlocks the matching locks on the specified inode and tries to apply pending locks.
	/// @param context Filesystem context.
	/// @param fsOpContext The operation context carrying a transaction in KV backends.
	/// @param type Type of locks to unlock (kFlock, kPosix).
	/// @param inode inode number on which to unlock locks.
	/// @param applied Vector to be filled with the owners of the unlocked locks.
	virtual int locksUnlockInode(const FsContext &context,
	                             const FilesystemOperationContext &fsOpContext, uint8_t type,
	                             inode_t inode, std::vector<FileLocks::Owner> &applied) = 0;

	/// Removes a pending lock matching the provided parameters.
	/// @param context Filesystem context.
	/// @param fsOpContext The operation context carrying a transaction in KV backends.
	/// @param type Type of lock to operate on (kFlock, kPosix).
	/// @param ownerid Owner identifier provided by the client (FUSE owner typically).
	/// @param sessionid Session id of the client that enqueued the lock.
	/// @param inode Inode number on which the pending lock was queued.
	/// @param reqid Request id (used to identify interruptible requests).
	virtual int locksRemovePending(const FsContext &context,
	                               const FilesystemOperationContext &fsOpContext, uint8_t type,
	                               uint64_t ownerid, uint32_t sessionid, inode_t inode,
	                               uint64_t reqid) = 0;
};

// Global filesystem operations instance.
// This global unique_ptr is initialized once at startup (before any FS calls) and set to a single
// concrete implementation for the process lifetime. It must not be reassigned and its dynamic type
// remains stable, so callers may assume one immutable implementation.
inline std::unique_ptr<IFilesystemOperations> gFSOperations = nullptr;
