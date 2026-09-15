/*
   Copyright 2026 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cstdint>

#include "common/type_defs.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_operation_context.h"
#include "master/hstring.h"

/// Edge-only restore helpers used by EdgeUndoRecorder to apply rollback to the in-memory
/// directory tree.
///
/// These helpers implement the section-local contract for the EDGE section: they rebuild
/// directory topology (the parent's entry map and entries hash, the child's parent
/// back-pointers, the directory link count, and recursively aggregated directory stats) plus
/// inode-keyed trash/reserved paths. They deliberately do not touch node bodies (mode, times,
/// length, checksums), which are owned and restored by the NODE section. The full metadata
/// checksum is recomputed once after all sections load, so the helpers do not maintain per-node
/// checksums.
///
/// Unlike the runtime link()/removeEdge() operations, these helpers are signal-free: they must
/// not emit edgeChangedSignal/edgeRemovedSignal/nodeChangedSignal. During load the metadata
/// writer is already active on a master, so emitting a signal here would re-enqueue a spurious
/// EDGE_ write and corrupt the persisted image. They mirror the in-memory attach performed by
/// MetadataBackendForkless::loadEdge() (which also ignores case-insensitive lower-case entries).
namespace metadata::edges {

/// Restores an edge so that (parentId, name) maps to childId in memory.
///
/// Resolves parentId to a directory and childId to a node (both must already exist; the NODE
/// section is rolled back before edges). If an edge with name already maps to childId, the call
/// is a no-op. If it maps to a different child, that edge is detached first. Then the child is
/// attached under name: inserted into the parent's entry map, linked back via child->parents,
/// the directory link count is bumped for directory children, and the child's stats are added to
/// the parent subtree. Parent id 0 is invalid here and reserved by the undo key encoding for
/// detached paths.
///
/// @param fsOpContext Filesystem operation context.
/// @param parentId    Inode of the parent directory.
/// @param childId     Inode of the child node the edge must point to.
/// @param name        Edge name (filename component).
/// @return kOpSuccess on success, kOpFailure when the parent is missing/not a directory, the
///         child is missing, or the entry insert fails.
int8_t restoreLoadedEdge(const FilesystemOperationContext &fsOpContext, inode_t parentId,
                         inode_t childId, const HString &name);

/// Removes the edge (parentId, name) from memory if present.
///
/// Idempotent: returns success when the edge or its parent is already absent. On success the
/// entry is erased from the parent's map, the child's matching parent back-pointer is dropped,
/// the directory link count is decremented for directory children, and the child's stats are
/// subtracted from the parent subtree. Parent id 0 is invalid here and reserved by the undo key
/// encoding for detached paths.
///
/// @param fsOpContext Filesystem operation context.
/// @param parentId    Inode of the parent directory.
/// @param name        Edge name (filename component) to remove.
/// @return kOpSuccess on success or when already absent, kOpFailure when the parent exists but is
///         not a directory.
int8_t removeLoadedEdge(const FilesystemOperationContext &fsOpContext, inode_t parentId,
                        const HString &name);

/// Restores one inode-keyed path to the in-memory trash or reserved container.
///
/// Resolves the inode to an existing node and verifies that its type matches nodeType, which
/// must be either trash or reserved. Any previous detached entry for the inode is removed before
/// the new path is inserted, keeping the detached-path indexes, node counters, and space counters
/// consistent.
///
/// @param fsOpContext Filesystem operation context.
/// @param inode       Inode of the detached node.
/// @param nodeType    Trash or reserved type selecting the destination container.
/// @param path        Detached path to restore.
/// @return kOpSuccess on success, kOpFailure when the inode is missing, its type is invalid, or
///         its previous detached entry cannot be removed.
int8_t restoreLoadedDetachedPath(const FilesystemOperationContext &fsOpContext, inode_t inode,
                                 FSNodeType nodeType, const HString &path);

/// Removes one inode from either in-memory detached container, if present.
///
/// Idempotent: returns success when the inode is already absent from both containers. Existing
/// entries are removed from the trash/reserved containers and their auxiliary indexes, and the
/// corresponding node and space counters are decremented.
///
/// @param fsOpContext Filesystem operation context.
/// @param inode       Inode of the detached node to remove.
/// @return kOpSuccess on success or when already absent, kOpFailure when an existing detached
///         entry does not resolve to a file node.
int8_t removeLoadedDetachedPath(const FilesystemOperationContext &fsOpContext, inode_t inode);

}  // namespace metadata::edges
