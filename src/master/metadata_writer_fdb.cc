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
	// Key
	static constexpr size_t kChunkKeySize =
	    kChunkKeyPrefix.size() + sizeof(chunkId) + sizeof(version);
	static kv::Key key(kChunkKeySize);
	std::memcpy(key.data(), kChunkKeyPrefix.data(), kChunkKeyPrefix.size());
	uint8_t *ptr = key.data() + kChunkKeyPrefix.size();
	put64bit(&ptr, chunkId);
	put32bit(&ptr, version);

	// Value
	kv::Value value(sizeof(lockedTo) + sizeof(lockId));
	ptr = value.data();
	put32bit(&ptr, lockedTo);
	put32bit(&ptr, lockId);

	txn->set(key, value);
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
	flushNoLock();
}

size_t MetadataWriterFDB::pendingCount() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return pendingUpdates_.size();
}

void MetadataWriterFDB::flushNoLock() {
	if (pendingUpdates_.empty()) { return; }

	auto transaction = kvEngine_->createReadWriteTransaction();

	// Process all updates in order
	for (const auto &update : pendingUpdates_) {
		update->applyEvent(transaction.get());
	}

	if (!transaction->commit()) {
		safs::log_err("Failed to flush {} metadata updates to FDB", pendingUpdates_.size());
	} else {
		safs::log_info("Flushed {} metadata updates to FDB", pendingUpdates_.size());
		pendingUpdates_.clear();
	}
}
