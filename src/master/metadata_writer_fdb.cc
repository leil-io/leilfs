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

#include "common/platform.h"

#include "master/metadata_writer_fdb.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "common/datapack.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/kv_common_keys.h"
#include "master/metadata_checkpoint_manager.h"
#include "master/metadata_section_undo_recorder.h"

ChunkUpdateEvent::ChunkUpdateEvent(uint64_t _chunkId, uint32_t _version, uint32_t _lockedTo,
                                   uint32_t _lockId)
    : chunkId(_chunkId), version(_version), lockedTo(_lockedTo), lockId(_lockId) {}

void ChunkUpdateEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("ChunkUpdateEvent requires a valid transaction in the context");
		return;
	}

	// Key: CHNL_<ChunkId>
	kv::Key key = kv::encodeKeyBE(kChunkLatestKeyPrefix, chunkId);

	// Value: <chunkVersion><lockedTo><lockId>
	kv::Value value(sizeof(version) + sizeof(lockedTo) + sizeof(lockId));
	uint8_t *ptr = value.data();
	put32bit(&ptr, version);
	put32bit(&ptr, lockedTo);
	put32bit(&ptr, lockId);

	if (context.checkpointManager != nullptr && context.checkpointVersion > 0) {
		MetadataMutation mutation = ChunkSetMutation{
		    .chunkId = chunkId,
		    .liveKey = key,
		};

		context.checkpointManager->recordPreMutation(
		    MetadataMutationContext{
		        .transaction = context.transaction,
		        .checkpointVersion = context.checkpointVersion,
		    },
		    mutation);
	}

	context.transaction->set(key, value);
}

ChunkRemoveEvent::ChunkRemoveEvent(uint64_t _chunkId) : chunkId(_chunkId) {}

void ChunkRemoveEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("ChunkRemoveEvent requires a valid transaction in the context");
		return;
	}

	// Key: CHNL_<ChunkId>
	kv::Key key = kv::encodeKeyBE(kChunkLatestKeyPrefix, chunkId);

	if (context.checkpointManager != nullptr && context.checkpointVersion > 0) {
		// ChunkSetMutation records the pre-image (or a tombstone) generically, which is exactly the
		// undo a removal needs: rollback restores the chunk that existed before this checkpoint.
		MetadataMutation mutation = ChunkSetMutation{
		    .chunkId = chunkId,
		    .liveKey = key,
		};

		context.checkpointManager->recordPreMutation(
		    MetadataMutationContext{
		        .transaction = context.transaction,
		        .checkpointVersion = context.checkpointVersion,
		    },
		    mutation);
	}

	context.transaction->remove(key);
}

NodeUpdateEvent::NodeUpdateEvent(FSNode *_node)
    : nodeId(_node->id), serializedNode(_node->serializedSize()) {
	uint8_t *ptr = serializedNode.data();
	_node->serialize(&ptr);
}

void NodeUpdateEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("NodeUpdateEvent requires a valid transaction in the context");
		return;
	}

	// Key: NODE_<nodeId>
	auto key = kv::encodeKeyBE(kNodeKeyPrefix, nodeId);

	if (context.checkpointManager != nullptr && context.checkpointVersion > 0) {
		MetadataMutation mutation = NodeSetMutation{
		    .inode = nodeId,
		    .liveKey = key,
		};

		context.checkpointManager->recordPreMutation(
		    MetadataMutationContext{
		        .transaction = context.transaction,
		        .checkpointVersion = context.checkpointVersion,
		    },
		    mutation);
	}

	context.transaction->set(key, serializedNode);
}

NodeRemoveEvent::NodeRemoveEvent(inode_t _nodeId) : nodeId(_nodeId) {}

void NodeRemoveEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("NodeRemoveEvent requires a valid transaction in the context");
		return;
	}

	// Key: NODE_<nodeId>
	auto key = kv::encodeKeyBE(kNodeKeyPrefix, nodeId);

	if (context.checkpointManager != nullptr && context.checkpointVersion > 0) {
		MetadataMutation mutation = NodeRemoveMutation{
		    .inode = nodeId,
		    .liveKey = key,
		};

		context.checkpointManager->recordPreMutation(
		    MetadataMutationContext{
		        .transaction = context.transaction,
		        .checkpointVersion = context.checkpointVersion,
		    },
		    mutation);
	}

	context.transaction->remove(key);
}

FreeNodeUpdateEvent::FreeNodeUpdateEvent(inode_t _nodeId, uint32_t _timestamp)
    : nodeId(_nodeId), timestamp(_timestamp) {}

void FreeNodeUpdateEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("FreeNodeUpdateEvent requires a valid transaction in the context");
		return;
	}

	// Key: FREE_<nodeId>
	kv::Key key = kv::encodeKeyBE(kFreeKeyPrefix, nodeId);

	// timestamp == 0 indicates allocation (removal from free list), non-zero indicates freeing
	// (addition to free list with timestamp)
	if (timestamp == 0) {
		// Node is being allocated, remove from free list
		context.transaction->remove(key);
	} else {
		// Node is being freed, add to free list with timestamp
		kv::Value value(sizeof(timestamp));
		uint8_t *ptr = value.data();
		put32bit(&ptr, timestamp);
		context.transaction->set(key, value);
	}
}

EdgeUpdateEvent::EdgeUpdateEvent(inode_t _parentId, HString _name, inode_t _childId)
	: parentId(_parentId), name(std::move(_name)), childId(_childId) {}

void EdgeUpdateEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("EdgeUpdateEvent requires a valid transaction in the context");
		return;
	}

	// EDGE_<ParentId><Name>: <ChildId>. e.g.: EDGE_1999ChildName: 2535

	// Key: EDGE_<parentId><name>
	auto key = kv::encodeKeyBE(kEdgeKeyPrefix, parentId);
	kv::appendStr(key, name);

	if (context.checkpointManager != nullptr && context.checkpointVersion > 0) {
		MetadataMutation mutation = EdgeSetMutation{
		    .parentId = parentId,
		    .childId = childId,
		    .name = name,
		    .liveKey = key,
		};

		context.checkpointManager->recordPreMutation(
		    MetadataMutationContext{
		        .transaction = context.transaction,
		        .checkpointVersion = context.checkpointVersion,
		    },
		    mutation);
	}

	// Value: childId
	kv::Value value(kv::toBytesBE(childId));

	context.transaction->set(key, value);
}

EdgeRemoveEvent::EdgeRemoveEvent(inode_t _parentId, HString _name)
	: parentId(_parentId), name(std::move(_name)) {}

void EdgeRemoveEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("EdgeRemoveEvent requires a valid transaction in the context");
		return;
	}

	// Key: EDGE_<parentId><name>
	auto key = kv::encodeKeyBE(kEdgeKeyPrefix, parentId);
	kv::appendStr(key, name);

	if (context.checkpointManager != nullptr && context.checkpointVersion > 0) {
		MetadataMutation mutation = EdgeRemoveMutation{
		    .parentId = parentId,
		    .name = name,
		    .liveKey = key,
		};

		context.checkpointManager->recordPreMutation(
		    MetadataMutationContext{
		        .transaction = context.transaction,
		        .checkpointVersion = context.checkpointVersion,
		    },
		    mutation);
	}

	context.transaction->remove(key);
}

XAttrUpdateEvent::XAttrUpdateEvent(inode_t _inode, std::span<const uint8_t> _name,
                                   std::span<const uint8_t> _value)
    : inode(_inode), name(_name.begin(), _name.end()), value(_value.begin(), _value.end()) {}

void XAttrUpdateEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("XAttrUpdateEvent requires a valid transaction in the context");
		return;
	}

	// Key: XATR_<inode><attributeName>
	auto key = kv::encodeKeyBE(kXAttrKeyPrefix, inode);
	key.insert(key.end(), name.begin(), name.end());

	if (context.checkpointManager != nullptr && context.checkpointVersion > 0) {
		MetadataMutation mutation = XAttrSetMutation{
		    .inode = inode,
		    .name = name,
		    .liveKey = key,
		};

		context.checkpointManager->recordPreMutation(
		    MetadataMutationContext{
		        .transaction = context.transaction,
		        .checkpointVersion = context.checkpointVersion,
		    },
		    mutation);
	}

	// Value: raw attribute value bytes
	context.transaction->set(key, value);
}

XAttrRemoveEvent::XAttrRemoveEvent(inode_t _inode, std::span<const uint8_t> _name)
    : inode(_inode), name(_name.begin(), _name.end()) {}

void XAttrRemoveEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("XAttrRemoveEvent requires a valid transaction in the context");
		return;
	}

	// Key: XATR_<inode><attributeName>
	auto key = kv::encodeKeyBE(kXAttrKeyPrefix, inode);
	key.insert(key.end(), name.begin(), name.end());

	if (context.checkpointManager != nullptr && context.checkpointVersion > 0) {
		MetadataMutation mutation = XAttrRemoveMutation{
		    .inode = inode,
		    .name = name,
		    .liveKey = key,
		};

		context.checkpointManager->recordPreMutation(
		    MetadataMutationContext{
		        .transaction = context.transaction,
		        .checkpointVersion = context.checkpointVersion,
		    },
		    mutation);
	}

	context.transaction->remove(key);
}

XAttrInodeRemoveEvent::XAttrInodeRemoveEvent(inode_t _inode) : inode(_inode) {}

void XAttrInodeRemoveEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("XAttrInodeRemoveEvent requires a valid transaction in the context");
		return;
	}

	// Remove all keys with prefix XATR_<inode>. Use prefixEnd() for the exclusive
	// upper bound so a max-valued inode cannot overflow inode+1 and produce an
	// invalid range.
	auto startKey = kv::encodeKeyBE(kXAttrKeyPrefix, inode);
	auto endKey = kv::prefixEnd(startKey);

	if (context.checkpointManager != nullptr && context.checkpointVersion > 0) {
		MetadataMutation mutation = XAttrRangeRemoveMutation{
		    .inode = inode,
		    .rangeBegin = startKey,
		    .rangeEnd = endKey,
		};

		context.checkpointManager->recordPreMutation(
		    MetadataMutationContext{
		        .transaction = context.transaction,
		        .checkpointVersion = context.checkpointVersion,
		    },
		    mutation);
	}

	context.transaction->removeRange(startKey, endKey);
}

namespace {

// Key prefix for all quota limit rows of one owner: QUOT_<OwnerType:u8><OwnerId:inode_t BE>
kv::Key quotaOwnerPrefix(QuotaOwnerType ownerType, inode_t ownerId) {
	kv::Key key = kv::toBytes(kQuotasKeyPrefix);
	key.push_back(static_cast<uint8_t>(ownerType));
	kv::Value idBytes = kv::toBytesBE(ownerId);
	key.insert(key.end(), idBytes.begin(), idBytes.end());
	return key;
}

// Full quota row key: QUOT_<OwnerType:u8><OwnerId:inode_t BE><Rigor:u8><Resource:u8>
kv::Key quotaEntryKey(QuotaOwnerType ownerType, inode_t ownerId, QuotaRigor rigor,
                      QuotaResource resource) {
	kv::Key key = quotaOwnerPrefix(ownerType, ownerId);
	key.push_back(static_cast<uint8_t>(rigor));
	key.push_back(static_cast<uint8_t>(resource));
	return key;
}

}  // namespace

QuotaUpdateEvent::QuotaUpdateEvent(QuotaOwnerType _ownerType, inode_t _ownerId,
                                   std::vector<QuotaEntry> _entries)
    : ownerType(_ownerType), ownerId(_ownerId), entries(std::move(_entries)) {}

void QuotaUpdateEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("QuotaUpdateEvent requires a valid transaction in the context");
		return;
	}

	if (context.checkpointManager != nullptr && context.checkpointVersion > 0) {
		kv::Key ownerPrefix = quotaOwnerPrefix(ownerType, ownerId);
		MetadataMutation mutation = QuotaSetMutation{
		    .ownerType = ownerType,
		    .ownerId = ownerId,
		    .rangeBegin = ownerPrefix,
		    .rangeEnd = kv::prefixEnd(ownerPrefix),
		};

		context.checkpointManager->recordPreMutation(
		    MetadataMutationContext{
		        .transaction = context.transaction,
		        .checkpointVersion = context.checkpointVersion,
		    },
		    mutation);
	}

	// Persist only the soft/hard limit entries provided by the caller. Usage (kUsed) is rebuilt
	// from node loading and is intentionally excluded (it is not part of the quota checksum).
	for (const QuotaEntry &entry : entries) {
		kv::Key key = quotaEntryKey(ownerType, ownerId, entry.entryKey.rigor, entry.entryKey.resource);
		kv::Value value(kv::toBytesBE(static_cast<uint64_t>(entry.limit)));
		context.transaction->set(key, value);
	}
}

QuotaRemoveEvent::QuotaRemoveEvent(QuotaOwnerType _ownerType, inode_t _ownerId)
    : ownerType(_ownerType), ownerId(_ownerId) {}

void QuotaRemoveEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("QuotaRemoveEvent requires a valid transaction in the context");
		return;
	}

	kv::Key startKey = quotaOwnerPrefix(ownerType, ownerId);

	if (context.checkpointManager != nullptr && context.checkpointVersion > 0) {
		MetadataMutation mutation = QuotaRemoveMutation{
		    .ownerType = ownerType,
		    .ownerId = ownerId,
		    .rangeBegin = startKey,
		    .rangeEnd = kv::prefixEnd(startKey),
		};

		context.checkpointManager->recordPreMutation(
		    MetadataMutationContext{
		        .transaction = context.transaction,
		        .checkpointVersion = context.checkpointVersion,
		    },
		    mutation);
	}

	context.transaction->removeRange(startKey, kv::prefixEnd(startKey));
}

AclUpdateEvent::AclUpdateEvent(inode_t _inode, std::vector<uint8_t> _serializedAcl)
    : inode(_inode), serializedAcl(std::move(_serializedAcl)) {}

void AclUpdateEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("AclUpdateEvent requires a valid transaction in the context");
		return;
	}

	// Key: ACLS_<inode>
	auto key = kv::encodeKeyBE(kACLsKeyPrefix, inode);
	context.transaction->set(key, serializedAcl);
}

AclRemoveEvent::AclRemoveEvent(inode_t _inode) : inode(_inode) {}

void AclRemoveEvent::applyEvent(const MetadataWriteContext &context) {
	if (context.transaction == nullptr) {
		safs::log_err("AclRemoveEvent requires a valid transaction in the context");
		return;
	}

	// Key: ACLS_<inode>
	auto key = kv::encodeKeyBE(kACLsKeyPrefix, inode);
	context.transaction->remove(key);
}

MetadataWriterFDB::MetadataWriterFDB(kv::IKVEngine *kvEngine,
                                     MetadataCheckpointManager *checkpointManager,
                                     size_t backlogHighWatermark, bool asyncFlush)
    : kvEngine_(kvEngine),
      checkpointManager_(checkpointManager),
      asyncFlush_(asyncFlush),
      backlogHighWatermark_(backlogHighWatermark),
      backlogLogStep_(backlogHighWatermark),
      backlogLowWatermark_(backlogHighWatermark / 2),
      maxPending_(kDefaultMaxPending_) {
	if (asyncFlush_) { startWorker(); }
}

MetadataWriterFDB::~MetadataWriterFDB() {
	if (asyncFlush_) {
		// Signals the worker to drain the remaining updates and exit, then joins it. The worker
		// still uses kvEngine_/checkpointManager_, which outlive the writer (see the backend's
		// member declaration order), so its final commits are safe.
		stopWorker();
		return;
	}
	if (pendingCount() > 0) {
		safs::log_warn(
		    "MetadataWriterFDB destroyed with {} pending updates, attempting final flush",
		    pendingCount());
		// flushBatch() is exception-safe, so this cannot throw out of the destructor.
		(void)flush(FlushMode::kDrainUntilEmpty);
	}
}

void MetadataWriterFDB::enqueue(std::unique_ptr<IMetadataUpdateEvent> event) {
	if (event == nullptr) {
		safs::log_err("{}: received null event, skipping", __func__);
		return;
	}

	size_t depth = 0;
	bool shouldLog = false;
	{
		std::unique_lock<std::mutex> lock(mutex_);
		if (asyncFlush_) {
			// Bounded queue (backpressure): if the background worker cannot keep up -- e.g. FDB is
			// stalled -- block the caller until space frees. The changelog is the durability
			// record, so throttling here only paces the in-memory->FDB mirror; nothing is lost.
			if (pendingUpdates_.size() >= maxPending_) {
				safs::log_warn("MetadataWriterFDB queue full ({} >= {}), throttling until it drains",
				               pendingUpdates_.size(), maxPending_);
				spaceCv_.wait(lock, [this] { return stop_ || pendingUpdates_.size() < maxPending_; });
			}
		}
		pendingUpdates_.emplace_back(std::move(event));
		depth = pendingUpdates_.size();
		shouldLog = noteBacklogGrowthLocked(depth);
		// Wake the background worker (async mode). Backpressure (hard cap, maxPending_) and the
		// backlog health signal (soft high-watermark, logged) are complementary: the watermark
		// warns before the cap ever blocks.
		if (asyncFlush_) { workCv_.notify_one(); }
	}

	// Log outside the lock. FDB persistence is asynchronous and the changelog is the durable
	// source of truth, so a growing backlog means FDB is lagging, not that data is lost; it is a
	// health signal to act on (alert / failover) before memory is exhausted.
	if (shouldLog) {
		safs::log_err(
		    "FDB metadata writer backlog: {} pending updates; FDB persistence is lagging.", depth);
	}
}

bool MetadataWriterFDB::noteBacklogGrowthLocked(size_t depth) {
	if (depth < backlogHighWatermark_) { return false; }

	if (!backlogActive_) {
		backlogActive_ = true;
		nextBacklogLogThreshold_ = depth + backlogLogStep_;
		++backlogEscalations_;
		return true;  // first crossing of the high-watermark
	}

	if (depth >= nextBacklogLogThreshold_) {
		nextBacklogLogThreshold_ = depth + backlogLogStep_;
		++backlogEscalations_;
		return true;  // grew by another step while still backlogged
	}

	return false;
}

void MetadataWriterFDB::maybeLogBacklogRecovery() {
	size_t depth = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!backlogActive_ || pendingUpdates_.size() >= backlogLowWatermark_) { return; }
		backlogActive_ = false;
		nextBacklogLogThreshold_ = 0;
		depth = pendingUpdates_.size();
	}
	safs::log_info("FDB metadata writer backlog cleared: {} pending updates remaining", depth);
}

bool MetadataWriterFDB::isBacklogCritical() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return pendingUpdates_.size() >= backlogHighWatermark_;
}

uint64_t MetadataWriterFDB::backlogEscalationCount() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return backlogEscalations_;
}

bool MetadataWriterFDB::flush(FlushMode mode) {
	// Snapshot: flush only the updates queued when the call started. A batch may consume fewer
	// than kMaxUpdatesPerFlush_ events when the byte cap splits it, so track how many were
	// actually taken instead of assuming a fixed batch size.
	size_t updatesToFlush = pendingCount();

	while (updatesToFlush > 0) {
		const size_t batchSize = std::min(updatesToFlush, kMaxUpdatesPerFlush_);
		size_t consumed = 0;
		if (!flushBatch(batchSize, consumed)) { return false; }
		if (consumed == 0) { break; }  // queue drained concurrently; nothing left to take
		// consumed <= batchSize <= updatesToFlush, so this cannot underflow.
		updatesToFlush -= consumed;
	}

	if (mode == FlushMode::kSnapshot) {
		maybeLogBacklogRecovery();
		return true;
	}

	while (pendingCount() > 0) {
		size_t consumed = 0;
		if (!flushBatch(kMaxUpdatesPerFlush_, consumed)) { return false; }
		if (consumed == 0) { break; }
	}

	maybeLogBacklogRecovery();
	return true;
}

bool MetadataWriterFDB::flushAndWait() {
	// Synchronous writer (e.g. migration bootstrap): drain on the caller's thread.
	if (!asyncFlush_) { return flush(FlushMode::kDrainUntilEmpty); }

	std::unique_lock<std::mutex> lock(mutex_);
	// Observe only failures from this point on; ignore a stale failure from a previous caller.
	lastFlushFailed_ = false;
	// Cut any active group-commit linger short: the seal needs the queue empty and the pipeline idle
	// as soon as possible, not after the linger window elapses.
	drainNow_ = true;
	workCv_.notify_all();
	lingerCv_.notify_all();
	// Wait until the worker has committed everything: the pending queue is empty and no commit is
	// still in flight. The checkpoint seal then runs with the pipeline fully drained.
	drainedCv_.wait(lock, [this] {
		return stop_ || lastFlushFailed_ || (pendingUpdates_.empty() && inFlight_.empty());
	});
	// Re-arm the group-commit linger for the next interval now that this drain is complete.
	drainNow_ = false;
	return !lastFlushFailed_ && !stop_;
}

size_t MetadataWriterFDB::pendingCount() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return pendingUpdates_.size();
}

MetadataWriterFDB::UpdateQueue MetadataWriterFDB::takeBatch(size_t maxUpdates) {
	UpdateQueue batch;
	std::lock_guard<std::mutex> lock(mutex_);

	const size_t batchSize = std::min(pendingUpdates_.size(), maxUpdates);
	for (size_t i = 0; i < batchSize; ++i) {
		batch.push_back(std::move(pendingUpdates_.front()));
		pendingUpdates_.pop_front();
	}

	return batch;
}

void MetadataWriterFDB::restoreBatch(UpdateQueue updates) {
	std::lock_guard<std::mutex> lock(mutex_);
	while (!updates.empty()) {
		pendingUpdates_.push_front(std::move(updates.back()));
		updates.pop_back();
	}
}

void MetadataWriterFDB::applyBatch(UpdateQueue &batch, kv::IReadWriteTransaction *transaction,
                                   size_t &appliedOut) {
	appliedOut = 0;
	MetadataWriteContext context{
	    .transaction = transaction,
	    .checkpointManager = checkpointManager_,
	    .checkpointVersion =
	        checkpointManager_ != nullptr ? checkpointManager_->activeCheckpointVersion() : 0,
	};
	size_t applied = 0;
	for (const auto &update : batch) {
		update->applyEvent(context);
		++applied;

		// Stop before the transaction exceeds the backend's per-transaction size limit. The
		// measured size includes the per-first-touch undo rows applyEvent() may have written in
		// this same transaction. Always keep at least one event so an oversized event still
		// commits alone rather than being deferred forever. getApproximateSize() is a client-side
		// estimate, so polling it per event is cheap.
		if (applied < batch.size()) {
			const auto approxSize = transaction->getApproximateSize();
			if (approxSize.has_value() && *approxSize >= kTxnSoftLimitBytes_) { break; }
		}
	}
	appliedOut = applied;
}

bool MetadataWriterFDB::commitBatch(UpdateQueue &batch, size_t &appliedOut) {
	appliedOut = 0;
	if (batch.empty()) { return true; }

	// Apply and commit under try/catch: a throwing applyEvent()/commit() (FDB timeout,
	// serialization error, etc.) must not lose the batch. This function never takes from or
	// restores to the queue -- the caller owns the batch: it retries the whole batch on failure
	// and re-queues the tail deferred by the size cap (reported via appliedOut) on success.
	size_t applied = 0;
	try {
		auto transaction = kvEngine_->createReadWriteTransaction();
		applyBatch(batch, transaction.get(), applied);
		if (!transaction->commit()) {
			safs::log_err("Failed to flush {} metadata updates to FDB", applied);
			return false;
		}
	} catch (const std::exception &e) {
		safs::log_err("Exception while flushing {} metadata updates to FDB: {}", applied, e.what());
		return false;
	} catch (...) {
		safs::log_err("Unknown exception while flushing {} metadata updates to FDB", applied);
		return false;
	}

	// Commit succeeded: the first `applied` events are durable. The caller re-queues any deferred
	// tail (events beyond the size cap) so it flushes, in order, in a following batch.
	appliedOut = applied;
	safs::log_info("Flushed {} metadata updates to FDB", applied);
	return true;
}

bool MetadataWriterFDB::flushBatch(size_t maxUpdates, size_t &consumedOut) {
	consumedOut = 0;
	auto batch = takeBatch(maxUpdates);
	if (batch.empty()) { return true; }

	size_t applied = 0;
	if (!commitBatch(batch, applied)) {
		// Restore the whole batch to the front so it is retried, in order, on the next flush.
		restoreBatch(std::move(batch));
		return false;
	}

	// Re-queue any tail deferred by the size cap so it flushes (with its own undo capture) in a
	// following batch, preserving order.
	if (applied < batch.size()) {
		UpdateQueue tail;
		for (size_t i = applied; i < batch.size(); ++i) { tail.push_back(std::move(batch[i])); }
		restoreBatch(std::move(tail));
	}

	consumedOut = applied;
	return true;
}

void MetadataWriterFDB::startWorker() {
	workerThread_ = std::thread([this] { workerLoop(); });
}

void MetadataWriterFDB::stopWorker() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	workCv_.notify_all();
	spaceCv_.notify_all();
	drainedCv_.notify_all();
	lingerCv_.notify_all();  // cut an active group-commit window short instead of waiting it out
	if (workerThread_.joinable()) { workerThread_.join(); }
}

void MetadataWriterFDB::requeueInFlightLocked() {
	// Move every in-flight batch's events, oldest batch first, back to the FRONT of the pending
	// queue preserving submission order, then clear the pipeline. The abandoned futures may still
	// land, but each re-commit writes the same self-contained snapshots (idempotent), so the
	// in-order re-commit yields the correct final per-key state even if a newer commit already
	// landed before an older one failed. Caller holds mutex_.
	UpdateQueue events;
	for (auto &commit : inFlight_) {
		while (!commit.events.empty()) {
			events.push_back(std::move(commit.events.front()));
			commit.events.pop_front();
		}
	}
	inFlight_.clear();
	while (!events.empty()) {
		pendingUpdates_.push_front(std::move(events.back()));
		events.pop_back();
	}
}

void MetadataWriterFDB::workerLoop() {
	std::unique_lock<std::mutex> lock(mutex_);
	while (true) {
		workCv_.wait(lock,
		             [this] { return stop_ || !pendingUpdates_.empty() || !inFlight_.empty(); });
		if (stop_ && pendingUpdates_.empty() && inFlight_.empty()) { break; }

		// Group-commit linger (trickle only): if the pipeline is idle and the queue holds less than a
		// full batch, wait briefly so a trickle of enqueues coalesces into one fatter commit instead
		// of many tiny ones. Gated on inFlight_ being empty -- when commits are already in flight,
		// their duration is itself the accumulation window, so we never delay a reap under load. The
		// wait is on lingerCv_, which enqueue() does NOT signal (no per-enqueue wakeup storm); only
		// stop_ or a seal/shutdown drain (drainNow_) cuts the window short.
		if (!stop_ && !drainNow_ && inFlight_.empty() && !pendingUpdates_.empty() &&
		    pendingUpdates_.size() < kMaxUpdatesPerFlush_) {
			lingerCv_.wait_for(lock, std::chrono::milliseconds(kBatchLingerMs_),
			                   [this] { return stop_ || drainNow_; });
		}

		// Fill the pipeline: keep up to kMaxInFlight_ commitAsync() transactions in flight so commit
		// latency overlaps with more work. The txn build + applyEvent (incl. recordPreMutation FDB
		// reads) + commitAsync run WITHOUT the lock, so enqueue() never blocks on FDB.
		while (inFlight_.size() < kMaxInFlight_ && !pendingUpdates_.empty()) {
			UpdateQueue batch;
			const size_t batchSize = std::min(pendingUpdates_.size(), kMaxUpdatesPerFlush_);
			for (size_t i = 0; i < batchSize; ++i) {
				batch.push_back(std::move(pendingUpdates_.front()));
				pendingUpdates_.pop_front();
			}
			spaceCv_.notify_all();  // freed queue space for any blocked enqueuer
			lock.unlock();

			std::unique_ptr<kv::IReadWriteTransaction> transaction;
			std::unique_ptr<kv::ICommitFuture> future;
			size_t applied = 0;
			bool built = false;
			try {
				transaction = kvEngine_->createReadWriteTransaction();
				applyBatch(batch, transaction.get(), applied);
				future = transaction->commitAsync();
				built = true;
			} catch (const std::exception &e) {
				safs::log_err("Exception starting async commit of {} updates: {}; requeuing",
				              batch.size(), e.what());
			} catch (...) {
				safs::log_err("Unknown exception starting async commit; requeuing");
			}

			lock.lock();
			if (built) {
				// The size cap may have deferred a tail: return it to the FRONT, in order, so the
				// pipeline picks it up as a following batch. The in-flight commit then holds only
				// the events actually written into its transaction (needed so a failure requeue via
				// requeueInFlightLocked() re-commits exactly those events, in order).
				while (batch.size() > applied) {
					pendingUpdates_.push_front(std::move(batch.back()));
					batch.pop_back();
				}
				inFlight_.push_back(InFlightCommit{.transaction = std::move(transaction),
				                                   .future = std::move(future),
				                                   .events = std::move(batch)});
			} else {
				while (!batch.empty()) {
					pendingUpdates_.push_front(std::move(batch.back()));
					batch.pop_back();
				}
				lastFlushFailed_ = true;
				// Unblock a waiting flushAndWait(): its predicate observes lastFlushFailed_, but the
				// reap below only runs (and only it notifies) when the pipeline is non-empty. Without
				// this the seal would sleep forever on a build failure with nothing in flight.
				drainedCv_.notify_all();
				break;  // stop filling; reap / back off below
			}
		}

		// Reap the oldest in-flight commit in submission order. Block on getResult() WITHOUT the lock
		// so the producer can keep enqueuing while we wait for durability. Only the worker mutates
		// inFlight_, so front() stays valid across the unlock.
		if (!inFlight_.empty()) {
			kv::ICommitFuture *future = inFlight_.front().future.get();
			lock.unlock();
			int commitError = 0;
			bool retryable = false;
			const bool committed = future->getResult(&commitError, &retryable);
			lock.lock();

			if (committed) {
				safs::log_info("Flushed {} metadata updates to FDB", inFlight_.front().events.size());
				inFlight_.pop_front();
			} else {
				// Forkless is the sole writer, so a failed commit means FDB is transiently
				// unavailable -- the whole pipeline is affected. Requeue this batch and every newer
				// in-flight batch in order, then retry. See requeueInFlightLocked().
				safs::log_err(
				    "Async commit failed (err {}, retryable {}); requeuing {} in-flight batch(es)",
				    commitError, retryable, inFlight_.size());
				requeueInFlightLocked();
				lastFlushFailed_ = true;
			}

			spaceCv_.notify_all();
			if (pendingUpdates_.empty() && inFlight_.empty()) { drainedCv_.notify_all(); }

			if (!committed) {
				if (stop_) {
					safs::log_warn(
					    "MetadataWriterFDB stopping with {} unflushed updates after a commit failure",
					    pendingUpdates_.size());
					break;
				}
				// Back off so a persistent FDB failure does not hot-spin; wake immediately on stop.
				workCv_.wait_for(lock, std::chrono::milliseconds(kCommitRetryBackoffMs_),
				                 [this] { return stop_; });
			}
		}
	}
}
