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

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

#include "common/special_inode_defs.h"
#include "common/type_defs.h"
#include "kv/ikv_engine.h"
#include "master/metadata_chunk_undo_recorder.h"
#include "master/metadata_edge_undo_recorder.h"
#include "master/metadata_node_undo_recorder.h"
#include "master/metadata_xattr_undo_recorder.h"
#include "master/metadata_section_undo_recorder.h"

/// Snapshot descriptor bound to one metadata checkpoint boundary.
///
/// Groups the restore-critical global values that define a coherent checkpoint of the
/// metadata image. The checkpoint manager persists these values as individual restore keys
/// (META_MAX_INODE_ID, META_VERSION, META_NEXT_SESSION, META_NEXT_CHUNK_ID) when a checkpoint
/// is sealed, and reads them back in loadLatestCheckpoint(). The descriptor must always
/// describe the same durable state that the live section keys encode, which is why the
/// backend drains pending writer batches before sealing a checkpoint.
struct MetadataCheckpointDescriptor {
	inode_t maxInodeId{SPECIAL_INODE_ROOT};  ///< Highest allocated inode id.
	uint64_t metadataVersion{1};  ///< Metadata version; also used as the checkpoint version id.
	uint32_t nextSessionId{1};    ///< Next client session id to assign.
	uint64_t nextChunkId{0};      ///< Next chunk id to assign.
};

/// Coordinates the checkpoint lifecycle for the forkless metadata backend.
///
/// The forkless backend keeps the newest metadata state in live FDB keys (NODE_, EDGE_,
/// FREE_, XATR_, CHNL_). This manager owns the durable bookkeeping that turns that live
/// image into well-defined checkpoint boundaries:
///
/// - the checkpoint descriptor with the restore-critical global values;
/// - the ordered catalog of retained sealed checkpoint versions (META_CHECKPOINT_VERSIONS).
///
/// Lifecycle: fs_storeall() stages the next boundary with beginCheckpoint(), drains the
/// pending writer batches, and then calls sealCheckpoint(). The active checkpoint version
/// advances only after the seal transaction commits successfully.
///
/// Retention: the last K sealed checkpoint versions are kept, where K is currently
/// gStoredPreviousBackMetaCopies + 1. Sealing a new checkpoint trims older versions from
/// the catalog and removes the per-checkpoint data associated with them.
///
/// Section-local restore support plugs in through the ISectionUndoRecorder extension
/// point: the manager routes pre-mutation notifications and restore requests to the
/// recorder owning the affected metadata section. No recorders are registered yet, so
/// these paths are currently no-ops.
class MetadataCheckpointManager {
public:
	/// Creates a manager bound to the given key-value engine.
	/// @param kvEngine Key-value engine used for all durable checkpoint state. Not owned.
	explicit MetadataCheckpointManager(kv::IKVEngine *kvEngine);

	/// Stages a checkpoint boundary while the current active interval is still draining.
	///
	/// The descriptor captures the restore-critical values at the boundary, but nothing is
	/// persisted yet and activeCheckpointVersion() does not change. A previously staged
	/// (but not yet sealed) checkpoint is replaced with a warning.
	///
	/// @param descriptor Restore-critical values captured at the checkpoint boundary.
	/// @return true (staging itself cannot fail).
	bool beginCheckpoint(const MetadataCheckpointDescriptor &descriptor);

	/// Seals a checkpoint: persists the descriptor and rotates the retained checkpoint
	/// catalog.
	///
	/// In a single read-write transaction this writes the descriptor restore keys, inserts
	/// descriptor.metadataVersion into META_CHECKPOINT_VERSIONS, trims versions that fall
	/// out of retention, and removes the per-checkpoint data of the trimmed versions.
	///
	/// On successful commit the active checkpoint version advances to
	/// descriptor.metadataVersion and subsequent updates are associated with the new
	/// checkpoint interval.
	///
	/// The caller must drain pending writer batches before sealing so that the descriptor
	/// describes the same durable state as the live section keys.
	///
	/// @param descriptor Restore-critical values to persist for this checkpoint.
	/// @return true if the checkpoint was committed; false on transaction failure, in which
	///         case the previous state is left unchanged and the seal can be retried.
	bool sealCheckpoint(const MetadataCheckpointDescriptor &descriptor);

	/// Loads the latest checkpoint descriptor and the retained checkpoint catalog from FDB.
	///
	/// Reads the descriptor restore keys (missing keys keep their defaults), loads
	/// META_CHECKPOINT_VERSIONS, and adopts the newest retained version as the active
	/// checkpoint version (0 when no checkpoint has been sealed yet). Any staged
	/// checkpoint is discarded.
	///
	/// Called during backend load, before section data is read.
	///
	/// @return Descriptor describing the newest sealed checkpoint.
	MetadataCheckpointDescriptor loadLatestCheckpoint();

	/// Refreshes the in-memory checkpoint catalog from FDB without reading the descriptor.
	///
	/// Reloads META_CHECKPOINT_VERSIONS and the active checkpoint version, resets per-interval
	/// recorder state, and discards any staged checkpoint. Unlike loadLatestCheckpoint() it does
	/// not touch the in-memory metadata globals.
	///
	/// Used on shadow->master promotion: the promoted node's in-memory metadata (kept current by
	/// changelog replay) is authoritative, so only the durable checkpoint catalog — which the old
	/// master kept sealing while this node was a shadow — needs to be re-read before this node
	/// seals its own checkpoints.
	void reloadDurableCheckpointState();

	/// Notifies the checkpoint layer that a live section key is about to be mutated.
	///
	/// Routes the typed mutation to the ISectionUndoRecorder owning the affected metadata
	/// section, allowing it to capture section-local state before the live key changes.
	/// Currently a no-op: no recorders are registered yet.
	///
	/// No-op also when context.transaction is null or context.checkpointVersion is 0
	/// (e.g. the bootstrap path deliberately writes without checkpoint bookkeeping).
	///
	/// @param context  Transaction and active checkpoint version of the flush in progress.
	/// @param mutation Typed description of the live-key mutation about to happen.
	void recordPreMutation(const MetadataMutationContext &context,
	                       const MetadataMutation &mutation);

	/// Restores one metadata section to the state it had at a sealed checkpoint version.
	///
	/// Delegates to the ISectionUndoRecorder registered for the section. Restore is
	/// strictly section-local: a recorder repairs only the state its section owns.
	/// Currently always returns false: no recorders are registered yet.
	///
	/// @param section       Metadata section to restore.
	/// @param targetVersion Sealed checkpoint version to restore to (must be non-zero).
	/// @return true if the section was restored; false for invalid arguments or when the
	///         section has no registered recorder.
	bool restoreSectionToCheckpointVersion(MetadataSectionKind section,
                                           uint64_t targetVersion);

	/// Returns the active checkpoint version: the last sealed checkpoint, not the one
	/// staged by beginCheckpoint().
	///
	/// Updates flushed while interval T is active are associated with checkpoint version T.
	/// The value is 0 before any checkpoint has been sealed (and during bootstrap).
	uint64_t activeCheckpointVersion() const { return activeCheckpointVersion_; }

	/// Inodes removed from the in-memory node table during the most recent node-section restore.
	///
	/// The forkless edge load consults this set to skip live edges whose child was rolled back
	/// away (post-checkpoint drift) instead of failing the EDGE section load. Returns an empty set
	/// when no node recorder is registered.
	const std::unordered_set<uint64_t> &nodesRemovedDuringRestore() const;

private:
	/// Registers the per-section undo recorders into recorders_.
	///
	/// Called once from the constructor. No recorders are registered yet, so the table is
	/// left empty and the section-local restore paths are currently no-ops.
	void initializeRecorders();

	/// Returns the recorder registered for the given section, or nullptr if none.
	/// @param section Metadata section to look up.
	ISectionUndoRecorder *recorderFor(MetadataSectionKind section);

	/// Resets the per-interval bookkeeping of every registered recorder.
	///
	/// Called after a checkpoint is sealed (and on load) so recorders start a fresh
	/// interval against the new active checkpoint version.
	void resetIntervalState();

	/// Writes the descriptor's restore-critical values as individual keys in the given
	/// transaction (META_MAX_INODE_ID, META_VERSION, META_NEXT_SESSION, META_NEXT_CHUNK_ID).
	///
	/// @param transaction Transaction to write into.
	/// @param descriptor  Values to persist.
	/// @return true on success; false if transaction is null.
	bool persistCheckpointDescriptor(kv::IReadWriteTransaction *transaction,
	                                 const MetadataCheckpointDescriptor &descriptor) const;

	/// Loads META_CHECKPOINT_VERSIONS into retainedCheckpointVersions_ and sets the active
	/// checkpoint version to the newest retained version (0 if none).
	///
	/// Lazily invoked by sealCheckpoint() when the catalog has not been loaded yet, and
	/// marks it loaded via checkpointVersionsLoaded_.
	void loadCheckpointVersions();

	/// Computes the next retained catalog from the current one plus a new checkpoint version.
	///
	/// Works on local outputs only and does not mutate any member, so the caller can apply the
	/// result after the seal transaction commits successfully.
	///
	/// @param newVersion New checkpoint version to insert.
	/// @param retained   Output: the catalog trimmed to the retention limit, ascending.
	/// @param dropped    Output: versions trimmed off the front (to be cleaned up).
	void computeRetainedCheckpointVersions(uint64_t newVersion, std::vector<uint64_t> &retained,
	                                       std::vector<uint64_t> &dropped) const;

	/// Remove keys associated with checkpoints that were dropped during trimming.
	///
	/// This is best-effort cleanup to limit growth of checkpoint-related keys as old snapshots
	/// fall out of retention.
	///
	/// @note: For some keyspaces (e.g. CHNU_ chunk undo), a full cleanup requires a prefix/range
	/// scan and removing all keys under that prefix.
	///
	/// @param transaction     Transaction used to remove keys.
	/// @param droppedVersions Checkpoint versions whose per-checkpoint data should be removed.
	/// @return kOpSuccess on success, kOpFailure on null transaction.
	int8_t removeDroppedCheckpointVersions(kv::IReadWriteTransaction *transaction,
	                                       const std::vector<uint64_t> &droppedVersions);

	/// Key-value engine used for all durable checkpoint state. Not owned.
	kv::IKVEngine *kvEngine_;

	/// Checkpoint staged by beginCheckpoint() and consumed by sealCheckpoint(); empty when
	/// no checkpoint is pending.
	std::optional<MetadataCheckpointDescriptor> pendingCheckpoint_;

	/// In-memory mirror of META_CHECKPOINT_VERSIONS: the retained sealed checkpoint
	/// versions in ascending order.
	std::vector<uint64_t> retainedCheckpointVersions_;

	/// Recorders indexed by MetadataSectionKind; null entries mean the section has no
	/// registered recorder. Recorders are not owned by this array.
	std::array<ISectionUndoRecorder *, kMetadataSectionKindCount> recorders_{};

	std::unique_ptr<ChunkUndoRecorder> chunkUndoRecorder_;
	std::unique_ptr<NodeUndoRecorder> nodeUndoRecorder_;
	std::unique_ptr<EdgeUndoRecorder> edgeUndoRecorder_;
	std::unique_ptr<XAttrUndoRecorder> xattrUndoRecorder_;

	/// Last sealed checkpoint version; 0 before any checkpoint has been sealed.
	uint64_t activeCheckpointVersion_{0};

	/// Whether the retained checkpoint catalog has been loaded from FDB yet, so it is read
	/// at most once before the first seal.
	bool checkpointVersionsLoaded_{false};
};
