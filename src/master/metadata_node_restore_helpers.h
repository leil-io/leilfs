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
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <cstdint>

#include "common/type_defs.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_operation_context.h"

/// Node-only restore helpers used by NodeUndoRecorder to apply rollback to the in-memory node
/// table.
///
/// These helpers implement the section-local contract for the NODE section: they restore node
/// bodies and node-local accounting (per-type counters, open-file sessions, quotas, checksums,
/// and inode-pool state), but they deliberately do not rebuild directory topology owned by the
/// EDGE section. Replacement and removal are rejected when the target node still has parent
/// links or edge-owned directory entries attached, so a node restore can never implicitly
/// mutate another section's state.
namespace metadata::nodes {

/// Inserts a freshly deserialized node that does not yet exist in memory.
///
/// Attaches node-local accounting, inserts the node into the global node hash, and marks its
/// inode as acquired in the inode pool. Ownership of node is transferred to gMetadata on success.
///
/// @param fsOpContext Filesystem operation context.
/// @param node        Deserialized node to insert.
/// @return kOpSuccess on success, kOpFailure if the node type is unrecognized.
int8_t insertLoadedNode(const FilesystemOperationContext &fsOpContext, FSNode *node);

/// Restores a node pre-image, inserting it when absent or replacing the existing node in place.
///
/// When no node with restoredNode->id exists, behaves like insertLoadedNode(). Otherwise the
/// existing node is replaced only if the types are compatible (equal, or both file-like) and
/// the replacement is node-only: the current node must have no parent links, and a directory
/// must have no edge-owned entries. The current node's accounting is detached and it is
/// destroyed; the restored node's accounting is attached and it is inserted. The caseInsensitive
/// flag is carried over for directories. restoredNode is destroyed on any failure.
///
/// @param fsOpContext  Filesystem operation context.
/// @param restoredNode Deserialized pre-image to restore. Ownership is consumed in all paths.
/// @return kOpSuccess on success, kOpFailure on incompatible type or attached edge-owned state.
int8_t restoreLoadedNode(const FilesystemOperationContext &fsOpContext, FSNode *restoredNode);

/// Removes a node that the rollback determined should not exist at the target checkpoint.
///
/// Idempotent: returns success when the node is already absent. Refuses to remove the root
/// inode, a node that still has parent links, or a directory that still has entries (all
/// edge-owned state). On success detaches accounting, removes the node from the hash, releases
/// its inode back to the pool, and destroys it.
///
/// @param fsOpContext Filesystem operation context.
/// @param nodeId      Inode to remove.
/// @return kOpSuccess on success or when already absent, kOpFailure when removal is refused.
int8_t removeLoadedNode(const FilesystemOperationContext &fsOpContext, inode_t nodeId);

}  // namespace metadata::nodes
