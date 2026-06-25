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
#include <unordered_set>

#include "kv/ikv_engine.h"
#include "kv/kv_types.h"
#include "master/metadata_section_undo_recorder.h"

/// Section-local undo recorder for the node metadata section (NODE).
///
/// The forkless backend keeps only the newest node state in the live NODE_ keys, and that
/// image always drifts forward as the master creates, modifies, and removes nodes. To let a
/// later load (in particular shadow sync) reconstruct node state as of a sealed checkpoint
/// version, this recorder captures the pre-image of the first node mutation in each checkpoint
/// interval into cold undo rows, and replays them in reverse on restore.
///
/// Undo keyspace: NODEU_<checkpointVersion:u64><nodeId:inode_t> (kNodeUndoKeyPrefix). The value
/// is the serialized node pre-image, or an empty-value tombstone when the node did not exist
/// before the first mutation in the interval.
///
/// Lifecycle: MetadataWriterFDB calls beforeMutation() before a node live key is written or
/// removed, inside the same write transaction. The checkpoint manager calls
/// restoreToCheckpointVersion() during load to roll the live image back, dropCheckpointData()
/// when a checkpoint is trimmed from the retained catalog, and resetIntervalState() after a
/// checkpoint is sealed so first-touch tracking restarts for the next interval.
///
/// Section-local scope: this recorder restores node bodies and node-local accounting only,
/// delegating the actual in-memory replacement/removal to metadata_node_restore_helpers. It
/// must not rebuild directory topology owned by the EDGE section; see ISectionUndoRecorder and
/// the section-local restore contract in MetadataCheckpointManager.
class NodeUndoRecorder final : public ISectionUndoRecorder {
public:
	/// Creates a recorder bound to the given key-value engine.
	/// @param kvEngine Key-value engine used to open transactions during restore and to load
	///                 the retained checkpoint catalog. Not owned.
	explicit NodeUndoRecorder(kv::IKVEngine *kvEngine);

	/// @return MetadataSectionKind::Node, the routing key the checkpoint manager uses to
	///         dispatch node mutations and restore requests to this recorder.
	MetadataSectionKind sectionKind() const override { return MetadataSectionKind::Node; }

	/// Records the pre-image of a node mutation before its live key is overwritten or removed.
	///
	/// Only NodeSetMutation and NodeRemoveMutation are handled; any other mutation variant is
	/// logged and ignored. No-op when context.transaction is null or context.checkpointVersion
	/// is 0 (e.g. the bootstrap path, which must not fabricate undo history). Only the first
	/// touch of each node per interval is recorded, so the captured pre-image stays the
	/// interval-start image.
	///
	/// @param context  Active write transaction and current checkpoint version of the flush.
	/// @param mutation Must hold a NodeSetMutation or NodeRemoveMutation (inode and live NODE_ key).
	void beforeMutation(const MetadataMutationContext &context,
	                    const MetadataMutation &mutation) override;

	/// Rolls node state back to targetVersion by replaying retained undo intervals from newest
	/// to oldest.
	///
	/// Applies every retained checkpoint version >= targetVersion, including targetVersion
	/// itself: live updates in the active interval are tagged with the last sealed version, so
	/// the target interval also holds post-target changes that must be undone. Iteration stops
	/// once a version is < targetVersion.
	///
	/// @param targetVersion Sealed checkpoint version to restore to; must lie within the
	///                      retained [earliest, latest] range.
	/// @return true on success, including when no checkpoints are retained; false when
	///         targetVersion is outside the retained range or a single-checkpoint replay fails.
	bool restoreToCheckpointVersion(uint64_t targetVersion) override;

	/// Applies every undo row of a single checkpoint interval to the in-memory node table.
	///
	/// Paginates over NODEU_<checkpointVersion> and, per node, deletes a loaded node for
	/// tombstones or deserializes and restores the stored pre-image, via
	/// metadata_node_restore_helpers.
	///
	/// @param fsOpContext      Filesystem operation context forwarded to the restore helpers.
	/// @param checkpointVersion Interval whose undo rows are replayed.
	/// @return {entriesRestored, success}; success is false if any undo entry fails to apply.
	std::pair<uint64_t, bool> restoreSingleCheckpoint(const FilesystemOperationContext &fsOpContext,
	                                                  uint64_t checkpointVersion) override;

	/// Removes all undo rows of a checkpoint interval that fell out of retention.
	///
	/// @param transaction             Read-write transaction used for the range delete.
	/// @param droppedCheckpointVersion Interval whose NODEU_ rows are removed.
	/// @return kOpSuccess on success, kOpFailure when transaction is null.
	int8_t dropCheckpointData(kv::IReadWriteTransaction *transaction,
	                          uint64_t droppedCheckpointVersion) override;

	/// Clears the per-interval first-touch tracking. Called after a checkpoint is sealed so the
	/// next interval starts recording fresh pre-images.
	void resetIntervalState() override { touchedNodeIds_.clear(); }

	/// Inodes removed from the in-memory node table during the most recent rollback.
	///
	/// Populated by restoreToCheckpointVersion(): each tombstone undo entry that deletes a node
	/// records its inode here (the set is cleared at the start of every restore). The forkless
	/// edge load consults this set so it can skip live edges whose child was rolled back away
	/// (post-checkpoint drift) instead of failing the load; those edges are reconciled by edge
	/// rollback and changelog replay.
	const std::unordered_set<uint64_t> &removedDuringRestore() const { return removedDuringRestore_; }

private:
	/// Handles a NodeSetMutation: reads the current live value and records either the existing
	/// node pre-image, or a tombstone when the node is being created. Recorded once per node
	/// per interval.
	void beforeNodeSet(const MetadataMutationContext &context, const NodeSetMutation &mutation);

	/// Handles a NodeRemoveMutation: records the existing node pre-image so the removal can be
	/// undone. No-op when the live key is already absent. Recorded once per node per interval.
	void beforeNodeRemove(const MetadataMutationContext &context, const NodeRemoveMutation &mutation);

	/// Writes the undo row holding the serialized node pre-image for inode under
	/// checkpointVersion. Never overwrites an existing undo row, so the interval-start pre-image
	/// is preserved, and marks the inode as touched.
	void recordNodeUndoSet(kv::IReadWriteTransaction *transaction, uint64_t checkpointVersion,
	                       inode_t inode, const kv::Value &serializedNode);

	/// Writes an empty-value tombstone undo row for inode under checkpointVersion, recording
	/// that the node did not exist before the first mutation in the interval. Never overwrites
	/// an existing undo row, and marks the inode as touched.
	void recordNodeUndoRemove(kv::IReadWriteTransaction *transaction, uint64_t checkpointVersion,
	                          inode_t inode);

	/// Applies one undo row to in-memory state: removes the loaded node for an empty tombstone,
	/// otherwise deserializes the pre-image and restores it via metadata_node_restore_helpers.
	/// @return true on success, false if creation or restore fails.
	bool applyNodeUndoEntry(const FilesystemOperationContext &fsOpContext, inode_t nodeId,
	                        const kv::Value &undoValue);

	/// Key-value engine used for all durable undo state. Not owned.
	kv::IKVEngine *kvEngine_{nullptr};

	/// Inodes already captured in the active checkpoint interval (first-touch guard).
	std::unordered_set<uint64_t> touchedNodeIds_;

	/// Inodes deleted during the most recent restoreToCheckpointVersion() (see
	/// removedDuringRestore()). Cleared at the start of each restore.
	std::unordered_set<uint64_t> removedDuringRestore_;
};
