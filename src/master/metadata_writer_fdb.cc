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

#include "kv/itransaction.h"
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

MetadataWriterFDB::MetadataWriterFDB(kv::IKVEngine *kvEngine) : kvEngine_(kvEngine) {
	pendingUpdates_.reserve(kInitialSize_);  // Default reserve size, can be adjusted
}

void MetadataWriterFDB::enqueue(std::unique_ptr<IMetadataUpdateEvent> event) {
	if (event == nullptr) {
		safs::log_err("{}: received null event, skipping", __func__);
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	pendingUpdates_.emplace_back(std::move(event));
}

void MetadataWriterFDB::flush() {
	std::lock_guard<std::mutex> lock(mutex_);
	(void)flushNoLock();
}

bool MetadataWriterFDB::flushAll() {
	std::lock_guard<std::mutex> lock(mutex_);
	while (!pendingUpdates_.empty()) {
		if (!flushNoLock()) {
			// Stop on failure to avoid a tight loop; caller can retry later.
			return false;
		}
	}
	return true;
}

size_t MetadataWriterFDB::pendingCount() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return pendingUpdates_.size();
}

bool MetadataWriterFDB::flushNoLock() {
	if (pendingUpdates_.empty()) { return true; }

	auto transaction = kvEngine_->createReadWriteTransaction();

	// Process all updates in order in batches of kMaxUpdatesPerFlush_
	const size_t batchSize = std::min(pendingUpdates_.size(), kMaxUpdatesPerFlush_);
	for (size_t i = 0; i < batchSize; ++i) {
		pendingUpdates_[i]->applyEvent(transaction.get());
	}

	if (!transaction->commit()) {
		safs::log_err("Failed to flush {} metadata updates to FDB", batchSize);
		return false;
	}

	safs::log_info("Flushed {} metadata updates to FDB", batchSize);
	pendingUpdates_.erase(pendingUpdates_.begin(),
	                      pendingUpdates_.begin() + static_cast<std::ptrdiff_t>(batchSize));
	return true;
}
