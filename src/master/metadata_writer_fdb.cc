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
#include <utility>

#include "common/datapack.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/kv_common_keys.h"
#include "master/metadata_checkpoint_manager.h"

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

	context.transaction->set(key, value);
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

	context.transaction->removeRange(startKey, endKey);
}

MetadataWriterFDB::MetadataWriterFDB(kv::IKVEngine *kvEngine,
                                     MetadataCheckpointManager *checkpointManager)
    : kvEngine_(kvEngine), checkpointManager_(checkpointManager) {}

MetadataWriterFDB::~MetadataWriterFDB() {
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
	std::lock_guard<std::mutex> lock(mutex_);
	pendingUpdates_.emplace_back(std::move(event));
}

bool MetadataWriterFDB::flush(FlushMode mode) {
	size_t updatesToFlush = pendingCount();

	while (updatesToFlush > 0) {
		const size_t batchSize = std::min(updatesToFlush, kMaxUpdatesPerFlush_);
		if (!flushBatch(batchSize)) { return false; }
		updatesToFlush -= batchSize;
	}

	if (mode == FlushMode::kSnapshot) { return true; }

	while (pendingCount() > 0) {
		if (!flushBatch(kMaxUpdatesPerFlush_)) { return false; }
	}

	return true;
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

bool MetadataWriterFDB::flushBatch(size_t maxUpdates) {
	auto batch = takeBatch(maxUpdates);
	if (batch.empty()) { return true; }

	// Apply and commit under try/catch: a throwing applyEvent()/commit() (FDB timeout,
	// serialization error, etc.) must not destroy the batch, or the updates are lost
	// permanently. Restore the batch on any failure so it is retried on the next flush.
	try {
		auto transaction = kvEngine_->createReadWriteTransaction();

		MetadataWriteContext context{
		    .transaction = transaction.get(),
		    .checkpointManager = checkpointManager_,
		    .checkpointVersion =
		        checkpointManager_ != nullptr ? checkpointManager_->activeCheckpointVersion() : 0,
		};

		for (const auto &update : batch) { update->applyEvent(context); }

		if (!transaction->commit()) {
			safs::log_err("Failed to flush {} metadata updates to FDB", batch.size());
			restoreBatch(std::move(batch));
			return false;
		}
	} catch (const std::exception &e) {
		safs::log_err("Exception while flushing {} metadata updates to FDB: {}", batch.size(),
		              e.what());
		restoreBatch(std::move(batch));
		return false;
	} catch (...) {
		safs::log_err("Unknown exception while flushing {} metadata updates to FDB", batch.size());
		restoreBatch(std::move(batch));
		return false;
	}

	safs::log_info("Flushed {} metadata updates to FDB", batch.size());
	return true;
}
