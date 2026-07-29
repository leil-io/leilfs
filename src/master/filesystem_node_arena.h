/*
   Copyright 2026      Leil Storage OÜ

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

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "common/type_defs.h"

class FSNode;

/// KV-only bookkeeping for file chunk tables stored outside FSNodeFile.
///
/// In-memory Master and Shadow never create these entries. KV backends attach one to
/// each file-like node materialized in an operation arena and persist it inside NODE_.
struct OutOfLineChunkTableMeta {
	/// Sentinel for mutablePrefixEnd: no bulk operation fences the file.
	static constexpr uint32_t kNoMutablePrefix = 0xFFFFFFFF;

	uint32_t liveChunkCount{};
	uint32_t chunkTableSize{};
	/// Monotonic content revision used to detect a source table changing during a bulk copy.
	uint64_t chunkTableRevision{};
	/// Monotonic identity assigned to durable bulk operations targeting this inode.
	uint64_t bulkOperationGeneration{};
	/// While an operation is active, only indices below this exclusive bound may mutate.
	uint32_t mutablePrefixEnd{kNoMutablePrefix};
};

/// Per-operation owner of FSNode objects materialized from a KV backend.
///
/// The node-resolution seam hands out borrowed FSNode pointers, valid for the current
/// operation. In-memory builds back that borrow with gMetadata->nodeHash, which owns every
/// node. KV builds materialize nodes from the database on demand instead; the arena is the
/// owner behind their borrows: each materialized node is adopted here and destroyed when the
/// owning FilesystemOperationContext is torn down. One instance per inode within the
/// operation, so repeated resolves observe each other's mutations. In-memory builds never
/// register anything and pay nothing.
/// @see FilesystemOperationContext::nodeArena
class FSNodeArena {
public:
	FSNodeArena() = default;
	FSNodeArena(const FSNodeArena &) = delete;
	FSNodeArena &operator=(const FSNodeArena &) = delete;
	FSNodeArena(FSNodeArena &&other) noexcept;
	FSNodeArena &operator=(FSNodeArena &&other) noexcept;

	/// Destroys every owned node.
	~FSNodeArena();

	/// Entry for the inode: nullopt when unknown (caller materializes and adopts), the owned
	/// instance when pinned, nullptr when tombstoned by releaseAndTombstone().
	std::optional<FSNode *> lookup(inode_t inode) const;

	/// Takes ownership of node and returns the arena's instance for the inode.
	/// Callers must use the returned pointer: when the inode is already pinned, the incoming
	/// node is destroyed and the pinned instance returned; adopting over a tombstone re-pins.
	[[nodiscard]] FSNode *adopt(inode_t inode, FSNode *node);

	/// Returns the KV chunk-table metadata attached to @p inode, or nullptr when absent.
	OutOfLineChunkTableMeta *chunkTableMeta(inode_t inode);
	const OutOfLineChunkTableMeta *chunkTableMeta(inode_t inode) const;

	/// Returns the existing metadata or creates a default, unfenced entry.
	OutOfLineChunkTableMeta &ensureChunkTableMeta(inode_t inode);

	/// Replaces the metadata attached to @p inode.
	void setChunkTableMeta(inode_t inode, OutOfLineChunkTableMeta metadata);

	/// Drops ownership and chunk-table metadata, then tombstones the inode.
	/// A later resolve in the same operation returns null instead of a dangling pointer.
	/// Used by delete paths where the removal call destroys the node itself.
	void releaseAndTombstone(inode_t inode);

	/// Drops node ownership and tombstones the inode while retaining its out-of-line metadata.
	/// Delete paths use this before removeNode(), which still needs chunk-table metadata, then
	/// call releaseAndTombstone() after the node has been destroyed.
	void releaseNodeOwnershipAndTombstone(inode_t inode);

private:
	void destroyAll();

	/// Owned nodes by inode; a null value is a tombstone for a node deleted this operation.
	std::unordered_map<inode_t, FSNode *> nodes_;

	/// KV-only file chunk-table metadata. Empty for in-memory Master and Shadow operations.
	std::unordered_map<inode_t, OutOfLineChunkTableMeta> chunkTableMetadata_;
};
