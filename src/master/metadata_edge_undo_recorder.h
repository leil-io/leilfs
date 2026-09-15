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
#include <set>
#include <utility>

#include "kv/ikv_engine.h"
#include "kv/kv_types.h"
#include "master/hstring.h"
#include "master/metadata_section_undo_recorder.h"

/// Section-local undo recorder for directory edges and detached paths.
///
/// The forkless backend keeps only the newest directory topology in the live EDGE_ keys, and
/// that image always drifts forward as the master links, renames, and unlinks entries. To let a
/// later load (in particular shadow sync) reconstruct topology as of a sealed checkpoint version,
/// this recorder captures the pre-image of the first edge mutation in each checkpoint interval
/// into cold undo rows, and replays them in reverse on restore.
///
/// Directory undo keyspace: EDGEU_<checkpointVersion:u64><parentId:inode_t><name>. Its value is
/// the pre-image child id, or an empty tombstone. Detached paths use
/// EDGEU_<checkpointVersion><0><inode>; the value is the prior node-type byte followed by its path,
/// or an empty tombstone. This gives each detached inode one identity across renames and
/// trash/reserved transitions while its live state uses the MDS-compatible TRSH_PATH_/RSVD_PATH_
/// families.
///
/// Lifecycle: MetadataWriterFDB calls beforeMutation() before a live edge or detached-path key is
/// written or removed, inside the same write transaction. The checkpoint manager calls
/// restoreToCheckpointVersion() during load to roll the live section state back,
/// dropCheckpointData() when a checkpoint is trimmed from the retained catalog, and
/// resetIntervalState() after a checkpoint is sealed. The durable undo key is the authoritative
/// first-touch guard, so there is no recorder-local interval state to reset.
///
/// Section-local scope: this recorder rebuilds directory topology (entry maps, link counts and
/// aggregated directory stats) and detached-path containers, delegating the in-memory changes to
/// metadata_edge_restore_helpers. It must not restore node bodies owned by the NODE section, which
/// are rolled back first; see ISectionUndoRecorder and the section-local restore contract in
/// MetadataCheckpointManager.
class EdgeUndoRecorder final : public ISectionUndoRecorder {
public:
	using DetachedPathKeySet = std::set<inode_t>;

	/// Creates a recorder bound to the given key-value engine.
	/// @param kvEngine Key-value engine used to open transactions during restore and to load the
	///                 retained checkpoint catalog. Not owned.
	explicit EdgeUndoRecorder(kv::IKVEngine *kvEngine);

	/// @return MetadataSectionKind::Edge, the routing key the checkpoint manager uses to dispatch
	///         edge mutations and restore requests to this recorder.
	MetadataSectionKind sectionKind() const override { return MetadataSectionKind::Edge; }

	/// Records the pre-image of an edge or detached-path mutation before its live state changes.
	///
	/// Handles edge and detached-path set/remove mutations; any other variant is logged and
	/// ignored. No-op when context.transaction is null or context.checkpointVersion is 0 (e.g.
	/// bootstrap). Only the first touch of each durable identity per interval is recorded.
	///
	/// @param context  Active write transaction and current checkpoint version of the flush.
	/// @param mutation Must hold an edge or detached-path set/remove mutation.
	void beforeMutation(const MetadataMutationContext &context,
	                    const MetadataMutation &mutation) override;

	/// Rolls directory topology and detached paths back to targetVersion by replaying retained undo
	/// intervals from newest to oldest.
	///
	/// Applies every retained checkpoint version >= targetVersion, including targetVersion itself:
	/// live updates in the active interval are tagged with the last sealed version, so the target
	/// interval also holds post-target changes that must be undone. Iteration stops once a version
	/// is < targetVersion.
	///
	/// @param targetVersion Sealed checkpoint version to restore to; must lie within the retained
	///                      [earliest, latest] range.
	/// @return true on success, including when no checkpoints are retained; false when
	///         targetVersion is outside the retained range or a single-checkpoint replay fails.
	bool restoreToCheckpointVersion(uint64_t targetVersion) override;

	/// Returns the detached-path inode keys processed by the latest checkpoint restore.
	const DetachedPathKeySet &detachedPathsTouchedDuringRestore() const {
		return detachedPathsTouchedDuringRestore_;
	}

	/// Applies every undo row of a single checkpoint interval to the in-memory EDGE section state.
	///
	/// Paginates over EDGEU_<checkpointVersion> and restores directory edges and detached paths via
	/// metadata_edge_restore_helpers.
	///
	/// @param fsOpContext      Filesystem operation context forwarded to the restore helpers.
	/// @param checkpointVersion Interval whose undo rows are replayed.
	/// @return {entriesRestored, success}; success is false if any undo entry fails to apply.
	std::pair<uint64_t, bool> restoreSingleCheckpoint(const FilesystemOperationContext &fsOpContext,
	                                                  uint64_t checkpointVersion) override;

	/// Removes all undo rows of a checkpoint interval that fell out of retention.
	///
	/// @param transaction             Read-write transaction used for the range delete.
	/// @param droppedCheckpointVersion Interval whose EDGEU_ rows are removed.
	/// @return kOpSuccess on success, kOpFailure when transaction is null.
	int8_t dropCheckpointData(kv::IReadWriteTransaction *transaction,
	                          uint64_t droppedCheckpointVersion) override;

	/// No in-memory first-touch state is retained; durable undo keys are authoritative.
	/// Kept for the common recorder lifecycle interface.
	void resetIntervalState() override {}

private:
	/// Records the pre-image for one edge identified by (parentId, name), using the durable undo
	/// key as the first-touch guard. Shared by the set and remove mutation paths.
	void beforeEdgeMutation(const MetadataMutationContext &context, inode_t parentId,
	                        const HString &name, const kv::Key &liveKey);

	/// Writes the undo row for (parentId, name) under checkpointVersion, copying the current live
	/// child id or an empty-value tombstone when the live key is absent. Never overwrites an
	/// existing undo row, so the interval-start pre-image is preserved.
	void recordEdgeUndo(kv::IReadWriteTransaction *transaction, uint64_t checkpointVersion,
	                    inode_t parentId, const HString &name, const kv::Key &liveKey);

	/// Captures the trash/reserved path pre-image for an inode under a single durable identity.
	void beforeDetachedPathMutation(const MetadataMutationContext &context, inode_t inode);

	/// Writes the detached-path undo row for inode under checkpointVersion, copying the current
	/// trash/reserved path and node type or an empty-value tombstone when neither live key exists.
	/// Never overwrites an existing undo row, so the interval-start pre-image is preserved.
	void recordDetachedPathUndo(kv::IReadWriteTransaction *transaction, uint64_t checkpointVersion,
	                            inode_t inode);

	/// Key-value engine used for all durable undo state. Not owned.
	kv::IKVEngine *kvEngine_{nullptr};

	/// Exact detached-path inodes processed by the current/most recent checkpoint restore.
	DetachedPathKeySet detachedPathsTouchedDuringRestore_;
};
