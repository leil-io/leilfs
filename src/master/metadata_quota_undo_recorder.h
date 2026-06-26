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

#include "common/type_defs.h"
#include "kv/ikv_engine.h"
#include "kv/kv_types.h"
#include "master/metadata_section_undo_recorder.h"
#include "protocol/quota.h"

/// Section-local undo recorder for the quota metadata section (QUOT).
///
/// The forkless backend keeps only the newest quota limits in the live QUOT_ keys, and that image
/// drifts forward as the master runs setquota. To let a later load (in particular shadow sync)
/// reconstruct limits as of a sealed checkpoint version, this recorder captures the pre-image of
/// the first mutation of each owner in a checkpoint interval into cold undo rows, and replays them
/// in reverse on restore.
///
/// Only soft/hard limits are persisted and checksummed; usage (kUsed) is rebuilt from node loading
/// and excluded. Like xattrs, quota limits are NOT node-coupled, so without this recorder a
/// drifted QUOT_ image causes a shadow checksum mismatch (see test_shadow_quota_sync.sh).
///
/// Undo keyspace: QUOTU_<checkpointVersion:u64><ownerType:u8><ownerId:inode_t>
/// (kQuotaUndoKeyPrefix). Both quota mutations rewrite/remove the owner's whole QUOT_ range, so the
/// recorder captures the owner's full pre-image: a one-byte presence flag, then (when present) four
/// u64 limits in soft/hard x inodes/size order, or a single 0x00 tombstone byte when the owner had
/// no limits.
///
/// Lifecycle: MetadataWriterFDB calls beforeMutation() before an owner's limit rows are written or
/// removed, inside the same write transaction. The checkpoint manager calls
/// restoreToCheckpointVersion() during load to roll limits back, dropCheckpointData() when a
/// checkpoint is trimmed, and resetIntervalState() after a checkpoint is sealed.
///
/// Section-local scope: this recorder restores quota limits only, via gMetadata->quotaDatabase. It
/// does not touch nodes or usage; see ISectionUndoRecorder and the section-local restore contract
/// in MetadataCheckpointManager.
class QuotaUndoRecorder final : public ISectionUndoRecorder {
public:
	/// Creates a recorder bound to the given key-value engine.
	/// @param kvEngine Key-value engine used to open transactions during restore and to load the
	///                 retained checkpoint catalog. Not owned.
	explicit QuotaUndoRecorder(kv::IKVEngine *kvEngine);

	/// @return MetadataSectionKind::Quota, the routing key the checkpoint manager uses to dispatch
	///         quota mutations and restore requests to this recorder.
	MetadataSectionKind sectionKind() const override { return MetadataSectionKind::Quota; }

	/// Records the pre-image of an owner's limits before its QUOT_ rows are written or removed.
	///
	/// Handles QuotaSetMutation and QuotaRemoveMutation; any other variant is logged and ignored.
	/// No-op when context.transaction is null or context.checkpointVersion is 0 (e.g. bootstrap).
	/// Only the first touch of each owner per interval is recorded.
	///
	/// @param context  Active write transaction and current checkpoint version of the flush.
	/// @param mutation Must hold a QuotaSetMutation or QuotaRemoveMutation.
	void beforeMutation(const MetadataMutationContext &context,
	                    const MetadataMutation &mutation) override;

	/// Rolls quota limits back to targetVersion by replaying retained undo intervals from newest to
	/// oldest, then recomputes the quota checksum.
	///
	/// Applies every retained checkpoint version >= targetVersion, including targetVersion itself
	/// (see ChunkUndoRecorder for the rationale). Stops once a version is < targetVersion.
	///
	/// @param targetVersion Sealed checkpoint version to restore to; must lie within the retained
	///                      [earliest, latest] range.
	/// @return true on success, including when no checkpoints are retained; false when
	///         targetVersion is outside the retained range or a single-checkpoint replay fails.
	bool restoreToCheckpointVersion(uint64_t targetVersion) override;

	/// Applies every undo row of a single checkpoint interval to the in-memory quota database.
	///
	/// Paginates over QUOTU_<checkpointVersion> and, per owner, removes the owner for tombstones or
	/// restores its four soft/hard limits, via gMetadata->quotaDatabase.
	///
	/// @param fsOpContext      Unused for quota restore (kept for ISectionUndoRecorder parity).
	/// @param checkpointVersion Interval whose undo rows are replayed.
	/// @return {entriesRestored, success}.
	std::pair<uint64_t, bool> restoreSingleCheckpoint(const FilesystemOperationContext &fsOpContext,
	                                                  uint64_t checkpointVersion) override;

	/// Removes all undo rows of a checkpoint interval that fell out of retention.
	///
	/// @param transaction             Read-write transaction used for the range delete.
	/// @param droppedCheckpointVersion Interval whose QUOTU_ rows are removed.
	/// @return kOpSuccess on success, kOpFailure when transaction is null.
	int8_t dropCheckpointData(kv::IReadWriteTransaction *transaction,
	                          uint64_t droppedCheckpointVersion) override;

	/// Clears the per-interval first-touch tracking. Called after a checkpoint is sealed so the
	/// next interval starts recording fresh pre-images.
	void resetIntervalState() override { touchedOwners_.clear(); }

private:
	/// Records the pre-image of one owner once per interval (scanning its live QUOT_ range) and
	/// marks it as touched. Shared by the set and remove mutation paths.
	void beforeOwnerMutation(const MetadataMutationContext &context, QuotaOwnerType ownerType,
	                         inode_t ownerId, const kv::Key &rangeBegin, const kv::Key &rangeEnd);

	/// Writes the undo row for (ownerType, ownerId) under checkpointVersion, capturing the owner's
	/// current soft/hard limits from the live QUOT_ range (presence + four u64 values) or a
	/// single 0x00 tombstone byte when the owner has no live rows. Never overwrites an existing
	/// undo row, so the interval-start pre-image is preserved.
	void recordOwnerUndo(kv::IReadWriteTransaction *transaction, uint64_t checkpointVersion,
	                     QuotaOwnerType ownerType, inode_t ownerId, const kv::Key &rangeBegin,
	                     const kv::Key &rangeEnd);

	/// Key-value engine used for all durable undo state. Not owned.
	kv::IKVEngine *kvEngine_{nullptr};

	/// QUOT_<ownerType><ownerId> prefixes already captured in the active interval (first-touch
	/// guard), keyed by the owner-prefix bytes.
	std::unordered_set<std::string> touchedOwners_;
};
