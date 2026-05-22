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

ChunkUpdateEvent::ChunkUpdateEvent(uint64_t _chunkId, uint32_t _version, uint32_t _lockedTo,
                                   uint32_t _lockId)
    : chunkId(_chunkId), version(_version), lockedTo(_lockedTo), lockId(_lockId) {}

void ChunkUpdateEvent::applyEvent(kv::IReadWriteTransaction *txn) {
	// Key: CHNK_<ChunkId><ChunkVersion>
	kv::Key key = kv::encodeKeyBE(kChunkKeyPrefix, chunkId, version);

	// Value: <lockedTo><lockId>
	kv::Value value(sizeof(lockedTo) + sizeof(lockId));
	uint8_t *ptr = value.data();
	put32bit(&ptr, lockedTo);
	put32bit(&ptr, lockId);

	txn->set(key, value);
}

NodeUpdateEvent::NodeUpdateEvent(FSNode *_node)
    : nodeId(_node->id), serializedNode(_node->serializedSize()) {
	uint8_t *ptr = serializedNode.data();
	_node->serialize(&ptr);
}

void NodeUpdateEvent::applyEvent(kv::IReadWriteTransaction *txn) {
	// Key: NODE_<nodeId>
	auto key = kv::encodeKeyBE(kNodeKeyPrefix, nodeId);
	txn->set(key, serializedNode);
}

NodeRemoveEvent::NodeRemoveEvent(inode_t _nodeId) : nodeId(_nodeId) {}

void NodeRemoveEvent::applyEvent(kv::IReadWriteTransaction *txn) {
	// Key: NODE_<nodeId>
	auto key = kv::encodeKeyBE(kNodeKeyPrefix, nodeId);
	txn->remove(key);
}

FreeNodeUpdateEvent::FreeNodeUpdateEvent(inode_t _nodeId, uint32_t _timestamp)
    : nodeId(_nodeId), timestamp(_timestamp) {}

void FreeNodeUpdateEvent::applyEvent(kv::IReadWriteTransaction *txn) {
	// Key: FREE_<nodeId>
	kv::Key key = kv::encodeKeyBE(kFreeKeyPrefix, nodeId);

	// timestamp == 0 indicates allocation (removal from free list), non-zero indicates freeing
	// (addition to free list with timestamp)
	if (timestamp == 0) {
		// Node is being allocated, remove from free list
		txn->remove(key);
	} else {
		// Node is being freed, add to free list with timestamp
		kv::Value value(sizeof(timestamp));
		uint8_t *ptr = value.data();
		put32bit(&ptr, timestamp);
		txn->set(key, value);
	}
}

EdgeUpdateEvent::EdgeUpdateEvent(inode_t _parentId, HString _name, inode_t _childId)
	: parentId(_parentId), name(std::move(_name)), childId(_childId) {}

void EdgeUpdateEvent::applyEvent(kv::IReadWriteTransaction *txn) {
	// EDGE_<ParentId><Name>: <ChildId>. e.g.: EDGE_1999ChildName: 2535

	// Key: EDGE_<parentId><name>
	auto key = kv::encodeKeyBE(kEdgeKeyPrefix, parentId);
	kv::appendStr(key, name);

	// Value: childId
	kv::Value value(kv::toBytesBE(childId));

	txn->set(key, value);
}

EdgeRemoveEvent::EdgeRemoveEvent(inode_t _parentId, HString _name)
	: parentId(_parentId), name(std::move(_name)) {}

void EdgeRemoveEvent::applyEvent(kv::IReadWriteTransaction *txn) {
	// Key: EDGE_<parentId><name>
	auto key = kv::encodeKeyBE(kEdgeKeyPrefix, parentId);
	kv::appendStr(key, name);

	txn->remove(key);
}

XAttrUpdateEvent::XAttrUpdateEvent(inode_t _inode, std::span<const uint8_t> _name,
                                   std::span<const uint8_t> _value)
    : inode(_inode), name(_name.begin(), _name.end()), value(_value.begin(), _value.end()) {}

void XAttrUpdateEvent::applyEvent(kv::IReadWriteTransaction *txn) {
	// Key: XATR_<inode><attributeName>
	auto key = kv::encodeKeyBE(kXAttrKeyPrefix, inode);
	key.insert(key.end(), name.begin(), name.end());

	// Value: raw attribute value bytes
	txn->set(key, value);
}

XAttrRemoveEvent::XAttrRemoveEvent(inode_t _inode, std::span<const uint8_t> _name)
    : inode(_inode), name(_name.begin(), _name.end()) {}

void XAttrRemoveEvent::applyEvent(kv::IReadWriteTransaction *txn) {
	// Key: XATR_<inode><attributeName>
	auto key = kv::encodeKeyBE(kXAttrKeyPrefix, inode);
	key.insert(key.end(), name.begin(), name.end());

	txn->remove(key);
}

XAttrInodeRemoveEvent::XAttrInodeRemoveEvent(inode_t _inode) : inode(_inode) {}

void XAttrInodeRemoveEvent::applyEvent(kv::IReadWriteTransaction *txn) {
	// Remove all keys in range XATR_<inode> .. XATR_<inode+1>
	auto startKey = kv::encodeKeyBE(kXAttrKeyPrefix, inode);
	auto endKey = kv::encodeKeyBE(kXAttrKeyPrefix, static_cast<inode_t>(inode + 1));

	txn->removeRange(startKey, endKey);
}

MetadataWriterFDB::MetadataWriterFDB(kv::IKVEngine *kvEngine) : kvEngine_(kvEngine) {}

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

	auto transaction = kvEngine_->createReadWriteTransaction();

	for (const auto &update : batch) { update->applyEvent(transaction.get()); }

	if (!transaction->commit()) {
		safs::log_err("Failed to flush {} metadata updates to FDB", batch.size());
		restoreBatch(std::move(batch));
		return false;
	}

	safs::log_info("Flushed {} metadata updates to FDB", batch.size());
	return true;
}
