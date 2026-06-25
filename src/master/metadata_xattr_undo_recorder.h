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
#include <span>
#include <string>
#include <unordered_set>

#include "common/type_defs.h"
#include "kv/ikv_engine.h"
#include "kv/kv_types.h"
#include "master/metadata_section_undo_recorder.h"

/// Section-local undo recorder for the extended-attribute metadata section (XATR).
///
/// The forkless backend keeps only the newest xattrs in the live XATR_ keys, and that image
/// always drifts forward as the master sets and removes attributes. To let a later load (in
/// particular shadow sync) reconstruct xattrs as of a sealed checkpoint version, this recorder
/// captures the pre-image of the first mutation of each xattr in a checkpoint interval into cold
/// undo rows, and replays them in reverse on restore.
///
/// Unlike free inodes, xattrs are NOT coupled to node create/delete: node rollback does not
/// restore them, and the xattr checksum is part of the metadata checksum. Without this recorder a
/// drifted XATR_ image causes a shadow checksum mismatch (see test_shadow_xattr_sync_with_fdb.sh).
///
/// Undo keyspace: XATRU_<checkpointVersion:u64><inode:inode_t><name> (kXAttrUndoKeyPrefix). The
/// value is a one-byte presence flag followed by the pre-image value: 0x01 + value when the xattr
/// existed, or a single 0x00 tombstone byte when it did not. The flag is required because xattrs
/// may legitimately hold an empty value.
///
/// Lifecycle: MetadataWriterFDB calls beforeMutation() before an xattr live key is written or
/// removed, inside the same write transaction. The checkpoint manager calls
/// restoreToCheckpointVersion() during load to roll the live xattrs back, dropCheckpointData()
/// when a checkpoint is trimmed from the retained catalog, and resetIntervalState() after a
/// checkpoint is sealed so first-touch tracking restarts for the next interval.
///
/// Section-local scope: this recorder restores xattr entries only, via xattr_setattr() with
/// signal emits suppressed. It does not touch node bodies or topology; see ISectionUndoRecorder
/// and the section-local restore contract in MetadataCheckpointManager.
class XAttrUndoRecorder final : public ISectionUndoRecorder {
public:
	/// Creates a recorder bound to the given key-value engine.
	/// @param kvEngine Key-value engine used to open transactions during restore and to load the
	///                 retained checkpoint catalog. Not owned.
	explicit XAttrUndoRecorder(kv::IKVEngine *kvEngine);

	/// @return MetadataSectionKind::XAttr, the routing key the checkpoint manager uses to dispatch
	///         xattr mutations and restore requests to this recorder.
	MetadataSectionKind sectionKind() const override { return MetadataSectionKind::XAttr; }

	/// Records the pre-image of an xattr mutation before its live key(s) are written or removed.
	///
	/// Handles XAttrSetMutation and XAttrRemoveMutation (one xattr) and XAttrRangeRemoveMutation
	/// (all xattrs of an inode, e.g. on node deletion); any other variant is logged and ignored.
	/// No-op when context.transaction is null or context.checkpointVersion is 0 (e.g. bootstrap).
	/// Only the first touch of each xattr per interval is recorded.
	///
	/// @param context  Active write transaction and current checkpoint version of the flush.
	/// @param mutation Must hold an XAttrSetMutation, XAttrRemoveMutation or XAttrRangeRemoveMutation.
	void beforeMutation(const MetadataMutationContext &context,
	                    const MetadataMutation &mutation) override;

	/// Rolls xattrs back to targetVersion by replaying retained undo intervals from newest to
	/// oldest.
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

	/// Applies every undo row of a single checkpoint interval to the in-memory xattr tables.
	///
	/// Paginates over XATRU_<checkpointVersion> and, per xattr, removes it for tombstones or sets
	/// it to the stored pre-image value, via xattr_setattr() with signal emits suppressed.
	///
	/// @param fsOpContext      Unused for xattr restore (kept for ISectionUndoRecorder parity).
	/// @param checkpointVersion Interval whose undo rows are replayed.
	/// @return {entriesRestored, success}; success is false if any undo entry fails to apply.
	std::pair<uint64_t, bool> restoreSingleCheckpoint(const FilesystemOperationContext &fsOpContext,
	                                                  uint64_t checkpointVersion) override;

	/// Removes all undo rows of a checkpoint interval that fell out of retention.
	///
	/// @param transaction             Read-write transaction used for the range delete.
	/// @param droppedCheckpointVersion Interval whose XATRU_ rows are removed.
	/// @return kOpSuccess on success, kOpFailure when transaction is null.
	int8_t dropCheckpointData(kv::IReadWriteTransaction *transaction,
	                          uint64_t droppedCheckpointVersion) override;

	/// Clears the per-interval first-touch tracking. Called after a checkpoint is sealed so the
	/// next interval starts recording fresh pre-images.
	void resetIntervalState() override { touchedXAttrs_.clear(); }

private:
	/// Records the pre-image for one xattr identified by (inode, name) once per interval and marks
	/// it as touched. Shared by the set and single-remove mutation paths.
	void beforeXAttrKey(const MetadataMutationContext &context, inode_t inode,
	                    std::span<const uint8_t> name, const kv::Key &liveKey);

	/// Records the pre-image of every live xattr of an inode before an inode-wide removal, by
	/// scanning the [rangeBegin, rangeEnd) live range inside the write transaction.
	void beforeXAttrRange(const MetadataMutationContext &context, const kv::Key &rangeBegin,
	                      const kv::Key &rangeEnd);

	/// Writes the undo row for (inode, name) under checkpointVersion, copying the current live
	/// value (prefixed with a 0x01 presence byte) or a single 0x00 tombstone byte when the live
	/// key is absent. Never overwrites an existing undo row, so the interval-start pre-image is
	/// preserved.
	void recordXAttrUndo(kv::IReadWriteTransaction *transaction, uint64_t checkpointVersion,
	                     inode_t inode, std::span<const uint8_t> name, const kv::Key &liveKey);

	/// Key-value engine used for all durable undo state. Not owned.
	kv::IKVEngine *kvEngine_{nullptr};

	/// Live XATR_ keys already captured in the active checkpoint interval (first-touch guard).
	/// Keyed by the live-key bytes, which uniquely identify an xattr (inode, name).
	std::unordered_set<std::string> touchedXAttrs_;
};
