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
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"

#include "master/kv_connector_fdb.h"

#include "common/serialization.h"
#include "config/cfg.h"
#include "fdb/fdb_context.h"
#include "fdb/fdb_kv_engine.h"
#include "kv/itransaction.h"
#include "master/changelog.h"
#include "master/kv_common_keys.h"
#include "master/kv_connector_interface.h"
#include "master/filesystem_metadata.h"

bool KVConnectorFDB::init() {
	std::string clusterFile = cfg_getstring("FDB_CLUSTER_FILE", "");

	if (clusterFile.empty()) {
		safs::log_err("FDB_CLUSTER_FILE is not set, cannot initialize FoundationDB");
		return false;
	}

	fdbContext_ = fdb::FDBContext::create({clusterFile});

	if (!fdbContext_) {
		safs::log_err("Failed to initialize FoundationDB context");
		return false;
	}

	auto fdbDB = fdbContext_->getDB();

	if (!fdbDB) {
		safs::log_err("Failed to get FoundationDB database instance");
		return false;
	}

	kvEngine_ = std::make_shared<fdb::FDBKVEngine>(fdbDB);

	if (!kvEngine_) {
		safs::log_err("Failed to create FoundationDB KV Engine");
		return false;
	}

	return true;
}

uint64_t KVConnectorFDB::get64bitBE(const kv::Key &key, uint64_t defaultValue) {
	auto transaction = kvEngine_->createReadWriteTransaction();
	auto value = transaction->get(key);

	if (value.has_value()) {
		const uint8_t *data = value.value().data();
		return get64bit(&data);
	}

	return defaultValue;
}

uint32_t KVConnectorFDB::get32bitBE(const kv::Key &key, uint32_t defaultValue) {
	auto transaction = kvEngine_->createReadWriteTransaction();
	auto value = transaction->get(key);

	if (value.has_value()) {
		uint32_t result;  // NOLINT(cppcoreguidelines-init-variables)
		const uint8_t *data = value.value().data();
		get32bit(&data, result);
		return result;
	}

	return defaultValue;
}

void KVConnectorFDB::onDetainedAdded(inode_t inodeId, uint32_t timestamp) {
	safs::log_info("Detained added signal: {} -> {}", inodeId, timestamp);
	auto transaction = kvEngine_->createReadWriteTransaction();

	// Key
	auto key = kv::encodeKeyBE(kFreeKeyPrefix, inodeId);

	// Value
	kv::Value value(sizeof(timestamp));
	auto *ptr = value.data();
	put32bit(&ptr, timestamp);

	transaction->set(key, value);

	if (!transaction->commit()) {
		safs::log_err("Failed to store free node: {} -> {}", inodeId, timestamp);
	}
}

void KVConnectorFDB::onDetainedRemoved(inode_t inodeId) {
	safs::log_info("Detained removed signal: {}", inodeId);
	auto transaction = kvEngine_->createReadWriteTransaction();

	auto key = kv::encodeKeyBE(kFreeKeyPrefix, inodeId);

	transaction->remove(key);

	if (!transaction->commit()) { safs::log_err("Failed to remove free node: {}", inodeId); }
}

void KVConnectorFDB::onNextSessionIdChanged(uint32_t /*oldSessionId*/, uint32_t newSessionId) {
	auto transaction = kvEngine_->createReadWriteTransaction();
	kv::Key sessionKey{kv::toBytes(gMetadata->nextSessionId().getName())};
	kv::Value sessionValue;
	serialize(sessionValue, newSessionId);
	transaction->set(sessionKey, sessionValue);

	if (!transaction->commit()) { safs::log_err("Failed to store session ID: {}", newSessionId); }
}

void KVConnectorFDB::onChangelogEvent(const ChangelogEvent &event) {
	static kv::Key versionKey{kv::toBytes(kMetaVersionKey)};
	kv::Value serializedVersion;
	serialize(serializedVersion, event.version);
	auto transaction = kvEngine_->createReadWriteTransaction();
	transaction->set(versionKey, serializedVersion);

	if (!transaction->commit()) {
		safs::log_err("Failed to store changelog entry: {}", event.entry);
		return;
	}
}

void KVConnectorFDB::onNodeChanged(FSNode *node) {
	auto transaction = kvEngine_->createReadWriteTransaction();

	// Key
	auto key = kv::encodeKeyBE(kNodeKeyPrefix, node->id);

	// Value
	kv::Value value;
	value.resize(node->serializedSize());
	uint8_t *ptr = value.data();
	node->serialize(&ptr);
	transaction->set(key, value);

	if (!transaction->commit()) { safs::log_err("Failed to store node: {}", node->id); }
}

void KVConnectorFDB::onEdgeChanged(FSNodeDirectory *parent, FSNode *child,
                                   hstorage::Handle *handlePtr) {
	auto transaction = kvEngine_->createReadWriteTransaction();

	// Key
	auto key = kv::encodeKeyBE(kEdgeKeyPrefix, parent->id, child->id);

	// Value
	auto name = handlePtr->get();
	kv::Value value(name.size());
	std::memcpy(value.data(), name.data(), name.size());

	transaction->set(key, value);

	if (!transaction->commit()) {
		safs::log_err("Failed to store edge: {} -> {} : {}", parent->id, child->id, name);
	}
}

void KVConnectorFDB::onEdgeRemoved(inode_t parentId, inode_t childId) {
	auto transaction = kvEngine_->createReadWriteTransaction();

	auto key = kv::encodeKeyBE(kEdgeKeyPrefix, parentId, childId);

	transaction->remove(key);

	if (!transaction->commit()) {
		safs::log_err("Failed to remove edge: {} -> {}", parentId, childId);
	}
}
