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
#include <string>
#include <unordered_set>

#include "kv/ikv_engine.h"
#include "kv/kv_types.h"
#include "master/hstring.h"
#include "master/metadata_section_undo_recorder.h"

/// Section-local undo recorder for the edge metadata section (EDGE).
///
/// The forkless backend keeps only the newest directory topology in the live EDGE_ keys, and
/// that image always drifts forward as the master links, renames, and unlinks entries. To let a
/// later load (in particular shadow sync) reconstruct topology as of a sealed checkpoint version,
/// this recorder captures the pre-image of the first edge mutation in each checkpoint interval
/// into cold undo rows, and replays them in reverse on restore.
///
/// Undo keyspace: EDGEU_<checkpointVersion:u64><parentId:inode_t><name> (kEdgeUndoKeyPrefix). The
/// value is the pre-image child id (the prior EDGE_ value), or an empty-value tombstone when the
/// edge did not exist before the first mutation in the interval.
///
/// Lifecycle: MetadataWriterFDB calls beforeMutation() before an edge live key is written or
/// removed, inside the same write transaction. The checkpoint manager calls
/// restoreToCheckpointVersion() during load to roll the live topology back, dropCheckpointData()
/// when a checkpoint is trimmed from the retained catalog, and resetIntervalState() after a
/// checkpoint is sealed so first-touch tracking restarts for the next interval.
///
/// Section-local scope: this recorder rebuilds directory topology only (entry maps, link counts,
/// aggregated directory stats), delegating the actual in-memory attach/detach to
/// metadata_edge_restore_helpers. It must not restore node bodies owned by the NODE section,
/// which are rolled back first; see ISectionUndoRecorder and the section-local restore contract
/// in MetadataCheckpointManager.
class EdgeUndoRecorder final : public ISectionUndoRecorder {
public:
	/// Creates a recorder bound to the given key-value engine.
	/// @param kvEngine Key-value engine used to open transactions during restore and to load the
	///                 retained checkpoint catalog. Not owned.
	explicit EdgeUndoRecorder(kv::IKVEngine *kvEngine);

	/// @return MetadataSectionKind::Edge, the routing key the checkpoint manager uses to dispatch
	///         edge mutations and restore requests to this recorder.
	MetadataSectionKind sectionKind() const override { return MetadataSectionKind::Edge; }

	/// Records the pre-image of an edge mutation before its live key is overwritten or removed.
	///
	/// Handles EdgeSetMutation and EdgeRemoveMutation; any other mutation variant is logged and
	/// ignored. No-op when context.transaction is null or context.checkpointVersion is 0 (e.g. the
	/// bootstrap path, which must not fabricate undo history). Only the first touch of each edge
	/// per interval is recorded, so the captured pre-image stays the interval-start image.
	///
	/// @param context  Active write transaction and current checkpoint version of the flush.
	/// @param mutation Must hold an EdgeSetMutation or EdgeRemoveMutation (parent id, name, and the
	///                 live EDGE_ key).
	void beforeMutation(const MetadataMutationContext &context,
	                    const MetadataMutation &mutation) override;

	/// Rolls directory topology back to targetVersion by replaying retained undo intervals from
	/// newest to oldest.
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

	/// Applies every undo row of a single checkpoint interval to the in-memory directory tree.
	///
	/// Paginates over EDGEU_<checkpointVersion> and, per edge, removes the edge for tombstones or
	/// restores it to the stored pre-image child id, via metadata_edge_restore_helpers.
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

	/// Clears the per-interval first-touch tracking. Called after a checkpoint is sealed so the
	/// next interval starts recording fresh pre-images.
	void resetIntervalState() override { touchedEdges_.clear(); }

private:
	/// Records the pre-image for one edge identified by (parentId, name) once per interval and
	/// marks the edge as touched. Shared by the set and remove mutation paths.
	void beforeEdgeMutation(const MetadataMutationContext &context, inode_t parentId,
	                        const HString &name, const kv::Key &liveKey);

	/// Writes the undo row for (parentId, name) under checkpointVersion, copying the current live
	/// child id or an empty-value tombstone when the live key is absent. Never overwrites an
	/// existing undo row, so the interval-start pre-image is preserved.
	void recordEdgeUndo(kv::IReadWriteTransaction *transaction, uint64_t checkpointVersion,
	                    inode_t parentId, const HString &name, const kv::Key &liveKey);

	/// Key-value engine used for all durable undo state. Not owned.
	kv::IKVEngine *kvEngine_{nullptr};

	/// Live EDGE_ keys already captured in the active checkpoint interval (first-touch guard).
	/// Keyed by the live-key bytes, which uniquely identify an edge (parentId, name).
	std::unordered_set<std::string> touchedEdges_;
};
