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

/// Section-local undo recorder for the chunk metadata section (CHNK).
///
/// The forkless backend keeps only the newest chunk state in the live CHNL_ keys, and that
/// image always drifts forward as the master mutates chunks. To let a later load (in
/// particular shadow sync) reconstruct chunk state as of a sealed checkpoint version, this
/// recorder captures the pre-image of the first chunk mutation in each checkpoint interval
/// into cold undo rows, and replays them in reverse on restore.
///
/// Undo keyspace: CHNU_<checkpointVersion:u64><chunkId:u64> (kChunkUndoKeyPrefix). The value
/// is the serialized CHNL_ pre-image (chunkVersion, lockedTo, lockId), or an empty-value
/// tombstone when the chunk did not exist before the first mutation in the interval.
///
/// Lifecycle: MetadataWriterFDB calls beforeMutation() before a chunk live key is written,
/// inside the same write transaction. The checkpoint manager calls restoreToCheckpointVersion()
/// during load to roll the live image back, dropCheckpointData() when a checkpoint is trimmed
/// from the retained catalog, and resetIntervalState() after a checkpoint is sealed so
/// first-touch tracking restarts for the next interval.
///
/// Section-local scope: this recorder rebuilds chunk-table state only. It must not touch node
/// bodies, edges, or any other section's owned state; see ISectionUndoRecorder and the
/// section-local restore contract in MetadataCheckpointManager.
class ChunkUndoRecorder final : public ISectionUndoRecorder {
public:
	/// Creates a recorder bound to the given key-value engine.
	/// @param kvEngine Key-value engine used to open transactions during restore and to load
	///                 the retained checkpoint catalog. Not owned.
	explicit ChunkUndoRecorder(kv::IKVEngine *kvEngine);

	/// @return MetadataSectionKind::Chunk, the routing key the checkpoint manager uses to
	///         dispatch chunk mutations and restore requests to this recorder.
	MetadataSectionKind sectionKind() const override { return MetadataSectionKind::Chunk; }

	/// Records the pre-image of a chunk mutation before its live key is overwritten.
	///
	/// Only ChunkSetMutation is handled; any other mutation variant is logged and ignored.
	/// No-op when context.transaction is null or context.checkpointVersion is 0 (e.g. the
	/// bootstrap path, which must not fabricate undo history). Only the first touch of each
	/// chunk per interval is recorded, so the captured pre-image stays the interval-start image.
	///
	/// @param context  Active write transaction and current checkpoint version of the flush.
	/// @param mutation Must hold a ChunkSetMutation (chunk id and its live CHNL_ key).
	void beforeMutation(const MetadataMutationContext &context,
	                    const MetadataMutation &mutation) override;

	/// Rolls chunk state back to targetVersion by replaying retained undo intervals from
	/// newest to oldest.
	///
	/// Applies every retained checkpoint version >= targetVersion, including targetVersion
	/// itself: live updates in the active interval are tagged with the last sealed version, so
	/// the target interval also holds post-target changes that must be undone. Iteration stops
	/// once a version is < targetVersion.
	///
	/// @param targetVersion Sealed checkpoint version to restore to; must lie within the
	///                      retained [earliest, latest] range.
	/// @return true on success, including when no checkpoints are retained; false when
	///         targetVersion is outside the retained range.
	bool restoreToCheckpointVersion(uint64_t targetVersion) override;

	/// Applies every undo row of a single checkpoint interval to the in-memory chunk table.
	///
	/// Paginates over CHNU_<checkpointVersion> and, per chunk, calls chunk_restore_remove()
	/// for tombstones or chunk_restore_set() for stored pre-images.
	///
	/// @param fsOpContext      Unused for chunk restore; kept for ISectionUndoRecorder parity.
	/// @param checkpointVersion Interval whose undo rows are replayed.
	/// @return {entriesRestored, success}.
	std::pair<uint64_t, bool> restoreSingleCheckpoint(const FilesystemOperationContext &fsOpContext,
	                                                  uint64_t checkpointVersion) override;

	/// Removes all undo rows of a checkpoint interval that fell out of retention.
	///
	/// @param transaction             Read-write transaction used for the range delete.
	/// @param droppedCheckpointVersion Interval whose CHNU_ rows are removed.
	/// @return kOpSuccess on success, kOpFailure when transaction is null.
	int8_t dropCheckpointData(kv::IReadWriteTransaction *transaction,
	                          uint64_t droppedCheckpointVersion) override;

	/// Clears the per-interval first-touch tracking. Called after a checkpoint is sealed so the
	/// next interval starts recording fresh pre-images.
	void resetIntervalState() override { touchedChunkIds_.clear(); }

private:
	/// Handles a ChunkSetMutation: records the pre-image once per chunk per interval and marks
	/// the chunk as touched.
	void beforeChunkSet(const MetadataMutationContext &context, const ChunkSetMutation &mutation);

	/// Writes the undo row for chunkId under checkpointVersion, copying the current live value
	/// or an empty-value tombstone when the live key is absent. Never overwrites an existing
	/// undo row, so the interval-start pre-image is preserved.
	void recordChunkUndo(kv::IReadWriteTransaction *transaction, uint64_t checkpointVersion,
	                     uint64_t chunkId, const kv::Key &liveKey);

	/// Key-value engine used for all durable undo state. Not owned.
	kv::IKVEngine *kvEngine_{nullptr};

	/// Chunk ids already captured in the active checkpoint interval (first-touch guard).
	std::unordered_set<uint64_t> touchedChunkIds_;
};
