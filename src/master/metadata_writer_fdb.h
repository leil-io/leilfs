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

#include "common/platform.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "common/type_defs.h"
#include "kv/ikv_engine.h"
#include "master/filesystem_node_types.h"
#include "protocol/quota.h"

class MetadataCheckpointManager;

struct MetadataWriteContext {
	kv::IReadWriteTransaction *transaction{nullptr};
	MetadataCheckpointManager *checkpointManager{nullptr};

	// Snapshot of the active checkpoint interval used for first-touch undo capture.
	// This is passed explicitly so events do not need to query manager state.
	uint64_t checkpointVersion{0};
};

class IMetadataUpdateEvent {
public:
	IMetadataUpdateEvent() = default;
	IMetadataUpdateEvent(const IMetadataUpdateEvent &) = default;
	IMetadataUpdateEvent(IMetadataUpdateEvent &&) = default;
	IMetadataUpdateEvent &operator=(const IMetadataUpdateEvent &) = default;
	IMetadataUpdateEvent &operator=(IMetadataUpdateEvent &&) = default;

	virtual ~IMetadataUpdateEvent() = default;

	virtual void applyEvent(const MetadataWriteContext &context) = 0;
};

/// Update event types for different metadata sections
class ChunkUpdateEvent : public IMetadataUpdateEvent {
public:
	ChunkUpdateEvent(uint64_t _chunkId, uint32_t _version, uint32_t _lockedTo, uint32_t _lockId);
	~ChunkUpdateEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	uint64_t chunkId;
	uint32_t version;
	uint32_t lockedTo;
	uint32_t lockId;
};

/// Update event for node changes
class NodeUpdateEvent : public IMetadataUpdateEvent {
public:
	NodeUpdateEvent(FSNode *_node);
	~NodeUpdateEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t nodeId;
	kv::Value serializedNode;
};

/// Removal event for nodes
class NodeRemoveEvent : public IMetadataUpdateEvent {
public:
	NodeRemoveEvent(inode_t _nodeId);
	~NodeRemoveEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t nodeId;
};

class FreeNodeUpdateEvent : public IMetadataUpdateEvent {
public:
	FreeNodeUpdateEvent(inode_t _nodeId, uint32_t _timestamp = 0);
	~FreeNodeUpdateEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t nodeId;
	uint32_t timestamp;
};

class EdgeUpdateEvent : public IMetadataUpdateEvent {
public:
	EdgeUpdateEvent(inode_t _parentId, HString _name, inode_t _childId);
	~EdgeUpdateEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t parentId;
	HString name;
	inode_t childId;
};

class EdgeRemoveEvent : public IMetadataUpdateEvent {
public:
	EdgeRemoveEvent(inode_t _parentId, HString _name);
	~EdgeRemoveEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t parentId;
	HString name;
};

/// Update event for xattr creation or value change.
/// Writes XATR_<InodeId><AttributeName>: <AttributeValue> to FDB.
class XAttrUpdateEvent : public IMetadataUpdateEvent {
public:
	XAttrUpdateEvent(inode_t _inode, std::span<const uint8_t> _name,
	                 std::span<const uint8_t> _value);
	~XAttrUpdateEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t inode;
	std::vector<uint8_t> name;
	std::vector<uint8_t> value;
};

/// Removal event for a single xattr entry.
/// Removes XATR_<InodeId><AttributeName> from FDB.
class XAttrRemoveEvent : public IMetadataUpdateEvent {
public:
	XAttrRemoveEvent(inode_t _inode, std::span<const uint8_t> _name);
	~XAttrRemoveEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t inode;
	std::vector<uint8_t> name;
};

/// Removal event for all xattrs of an inode.
/// Removes the range XATR_<InodeId> .. XATR_<InodeId+1> from FDB.
class XAttrInodeRemoveEvent : public IMetadataUpdateEvent {
public:
	explicit XAttrInodeRemoveEvent(inode_t _inode);
	~XAttrInodeRemoveEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t inode;
};

/// Update event for quota limit creation or change for one owner.
/// Writes QUOT_<OwnerType><OwnerId><Rigor><Resource>: <Limit> for each provided entry.
/// Only soft/hard limit entries are persisted; usage (kUsed) is rebuilt from node loading and is
/// excluded from the quota checksum.
class QuotaUpdateEvent : public IMetadataUpdateEvent {
public:
	QuotaUpdateEvent(QuotaOwnerType _ownerType, inode_t _ownerId, std::vector<QuotaEntry> _entries);
	~QuotaUpdateEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	QuotaOwnerType ownerType;
	inode_t ownerId;
	std::vector<QuotaEntry> entries;
};

/// Removal event for all quota limits of one owner.
/// Removes the range QUOT_<OwnerType><OwnerId> .. (prefix end) from FDB.
class QuotaRemoveEvent : public IMetadataUpdateEvent {
public:
	QuotaRemoveEvent(QuotaOwnerType _ownerType, inode_t _ownerId);
	~QuotaRemoveEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	QuotaOwnerType ownerType;
	inode_t ownerId;
};

/// Metadata writer that preserves changelog ordering
class MetadataWriterFDB {
public:
	/// Controls which set of queued updates a flush operation must persist.
	///
	/// Snapshot mode flushes only the updates that were already queued when the call started.
	/// Drain-until-empty mode keeps flushing until the queue becomes empty, including updates
	/// enqueued while the flush is in progress.
	enum class FlushMode : uint8_t {
		kSnapshot,        ///< Flush only the initial snapshot of pending updates.
		kDrainUntilEmpty, ///< Keep flushing until no pending updates remain.
	};

	/// Default pending-update count at which the backlog is reported critical: far above what a
	/// healthy 100 ms flush cycle leaves queued, so crossing it means FDB persistence is lagging
	/// badly.
	static constexpr size_t kDefaultBacklogHighWatermark_ = 100000;

	/// @param kvEngine          Key-value engine used for flush transactions.
	/// @param checkpointManager Optional checkpoint manager (undo/first-touch capture).
	/// @param backlogHighWatermark Pending-update count at which the backlog is reported critical
	///        (see isBacklogCritical()). Injectable so tests can use a small threshold; defaults
	///        to kDefaultBacklogHighWatermark_.
	explicit MetadataWriterFDB(kv::IKVEngine *kvEngine,
	                           MetadataCheckpointManager *checkpointManager = nullptr,
	                           size_t backlogHighWatermark = kDefaultBacklogHighWatermark_);

	/// Flushes any remaining pending updates so they are not lost on shutdown/restart.
	~MetadataWriterFDB();

	// Non-copyable and non-movable (holds a mutex and a pending-update queue).
	MetadataWriterFDB(const MetadataWriterFDB &) = delete;
	MetadataWriterFDB &operator=(const MetadataWriterFDB &) = delete;
	MetadataWriterFDB(MetadataWriterFDB &&) = delete;
	MetadataWriterFDB &operator=(MetadataWriterFDB &&) = delete;

	/// Enqueue an update (thread-safe)
	void enqueue(std::unique_ptr<IMetadataUpdateEvent> event);

	/// Flushes pending updates that were queued when the call started (thread-safe).
	/// Intended for periodic/background flushing to avoid unbounded backlog growth.
	bool flush(FlushMode mode = FlushMode::kSnapshot);

	/// Get count of pending updates
	size_t pendingCount() const;

	/// Whether the pending-update backlog has reached the critical high-watermark, i.e. FDB
	/// persistence is lagging far behind the in-memory metadata. This is a health signal: the
	/// changelog remains the durable source of truth (see the note on the watermark constants),
	/// so a critical backlog is not data loss, but the master should be investigated / failed
	/// over before memory is exhausted. Intended for a health endpoint or monitoring.
	bool isBacklogCritical() const;

	/// Number of backlog escalation events logged so far (one per high-watermark crossing and
	/// per subsequent growth step). A monotonically increasing monitoring / test hook.
	uint64_t backlogEscalationCount() const;

private:
	using UpdateQueue = std::deque<std::unique_ptr<IMetadataUpdateEvent>>;

	UpdateQueue takeBatch(size_t maxUpdates);
	void restoreBatch(UpdateQueue updates);
	bool flushBatch(size_t maxUpdates, size_t &consumedOut);

	/// Updates backlog bookkeeping after the queue grew to `depth` (called under mutex_).
	/// Returns true when this growth should emit an escalation log.
	bool noteBacklogGrowthLocked(size_t depth);

	/// Logs a one-shot recovery message and clears the backlog state once the queue has drained
	/// back below the low-watermark. Called after a flush drains the queue.
	void maybeLogBacklogRecovery();

	kv::IKVEngine *kvEngine_;
	MetadataCheckpointManager *checkpointManager_;

	mutable std::mutex mutex_;
	UpdateQueue pendingUpdates_;

	// Backlog watermark bookkeeping (guarded by mutex_). FDB persistence is asynchronous: the
	// changelog is the durable source of truth and FDB is a materialized image of it, so an FDB
	// outage does not lose data (recovery replays the changelog) — but pendingUpdates_ grows while
	// FDB is unreachable. These fields turn that otherwise-silent growth into an escalating,
	// queryable health signal so the backlog can be acted on (alert / failover) before memory is
	// exhausted. Note the durability contract: the maximum tolerable FDB outage is bounded by the
	// changelog retention window; beyond it the FDB image must be re-materialized from metadata.
	bool backlogActive_{false};
	size_t nextBacklogLogThreshold_{0};
	uint64_t backlogEscalations_{0};

	// Critical high-watermark (from the constructor), the escalation step (re-log once the backlog
	// grows by this many more events past the last log), and the recovery low-watermark (hysteresis
	// to avoid log flapping around the high-watermark). Derived from backlogHighWatermark_.
	const size_t backlogHighWatermark_;
	const size_t backlogLogStep_;
	const size_t backlogLowWatermark_;

	constexpr static size_t kMaxUpdatesPerFlush_ = 1000;

	// Soft cap on a flush transaction's approximate size. While applying a batch, the real
	// transaction size (IReadWriteTransaction::getApproximateSize(), which counts the mutations,
	// the per-first-touch undo rows written in the same transaction, conflict ranges and
	// backend overhead) is polled after each event; once it reaches this limit the remaining
	// events are deferred to the next batch. Kept below FDB's ~10 MB per-transaction limit with
	// headroom for the single event that crosses the threshold. At least one event is always
	// applied so an oversized event still makes progress.
	constexpr static size_t kTxnSoftLimitBytes_ = 8UL * 1000 * 1000;
};
