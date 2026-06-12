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

#include <limits>

#include "master/kv_connector_fdb.h"

#include "common/serialization.h"
#include "config/cfg.h"
#include "fdb/fdb_context.h"
#include "fdb/fdb_kv_engine.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/changelog.h"
#include "master/filesystem_metadata.h"
#include "master/kv_common_keys.h"
#include "master/kv_connector_interface.h"

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

	// Bound every FDB transaction so a stuck op (e.g. an unreachable cluster) fails instead of
	// blocking the single-threaded master loop forever; ~5s matches FDB's per-transaction
	// server limit. The option's valid range is [0, INT_MAX] ms (0 disables), so read int64
	// and reject out-of-range values, which FDB would otherwise truncate, wrap, or disable.
	static constexpr int64_t kDefaultTxnTimeoutMs = 5000;
	const auto timeoutMs = cfg_getint64("FDB_TRANSACTION_TIMEOUT_MS", kDefaultTxnTimeoutMs);
	if (timeoutMs < 0 || timeoutMs > std::numeric_limits<int32_t>::max()) {
		safs::log_err("FDB_TRANSACTION_TIMEOUT_MS must be in [0, {}] ms (0 disables), got {}",
		              std::numeric_limits<int32_t>::max(), timeoutMs);
		return false;
	}
	if (timeoutMs > 0) {
		const auto encoded = kv::toBytesLE<int64_t>(timeoutMs);  // FDB Int option = 8-byte LE
		const auto err = fdbDB->setOption(
		    FDB_DB_OPTION_TRANSACTION_TIMEOUT,
		    std::string_view(reinterpret_cast<const char *>(encoded.data()), encoded.size()));
		if (err != 0) {
			// A non-zero timeout was requested but could not be applied; fail fast instead of
			// running unbounded, which would reintroduce the hang this guard prevents.
			safs::log_err("Failed to set FDB transaction timeout ({} ms): {}", timeoutMs,
			              fdb::DB::errorMsg(err));
			return false;
		}
		safs::log_info("FDB transaction timeout set to {} ms", timeoutMs);
	}

	// Optionally lower FDB's per-transaction size cap (mutations plus read/write conflict
	// ranges), which FDB enforces at commit with "Transaction exceeds byte limit". A lowered
	// cap reproduces unbounded-accumulation bugs at a small, test-friendly scale instead of
	// the default 10 MB. The bounds mirror the size_limit option's documented valid range:
	// 32 bytes up to 10000000, which is FDB's default and hard maximum, so the cap can only
	// be lowered, never raised. 0 (this config's default) keeps FDB's own limit.
	static constexpr int64_t kMinTxnSizeLimitBytes = 32;
	static constexpr int64_t kMaxTxnSizeLimitBytes = 10'000'000;
	const auto sizeLimitBytes = cfg_getint64("FDB_TRANSACTION_SIZE_LIMIT_BYTES", 0);
	if (sizeLimitBytes != 0 &&
	    (sizeLimitBytes < kMinTxnSizeLimitBytes || sizeLimitBytes > kMaxTxnSizeLimitBytes)) {
		safs::log_err(
		    "FDB_TRANSACTION_SIZE_LIMIT_BYTES must be 0 (FDB default) or in "
		    "[{}, {}] bytes, got {}",
		    kMinTxnSizeLimitBytes, kMaxTxnSizeLimitBytes, sizeLimitBytes);
		return false;
	}
	if (sizeLimitBytes != 0) {
		const auto encoded = kv::toBytesLE<int64_t>(sizeLimitBytes);  // FDB Int option = 8-byte LE
		const auto err = fdbDB->setOption(
		    FDB_DB_OPTION_TRANSACTION_SIZE_LIMIT,
		    std::string_view(reinterpret_cast<const char *>(encoded.data()), encoded.size()));
		if (err != 0) {
			// A custom limit was requested but could not be applied; fail fast rather than run
			// with a limit other than the operator asked for.
			safs::log_err("Failed to set FDB transaction size limit ({} bytes): {}", sizeLimitBytes,
			              fdb::DB::errorMsg(err));
			return false;
		}
		safs::log_info("FDB transaction size limit set to {} bytes", sizeLimitBytes);
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

	// EDGE_<ParentId><Name>: <ChildId>. e.g.: EDGE_1999ChildName: 2535

	// Key
	auto key = kv::encodeKeyBE(kEdgeKeyPrefix, parent->id);
	kv::appendStr(key, handlePtr->get());

	// Value
	kv::Value value(kv::toBytesBE(child->id));

	transaction->set(key, value);

	if (!transaction->commit()) {
		safs::log_err("Failed to store edge: {} -> {} : {}", parent->id, child->id,
		              handlePtr->get());
	}
}

void KVConnectorFDB::onEdgeRemoved(inode_t parentId, const HString &edgeName) {
	auto transaction = kvEngine_->createReadWriteTransaction();

	auto key = kv::encodeKeyBE(kEdgeKeyPrefix, parentId);
	kv::appendStr(key, edgeName);

	transaction->remove(key);

	if (!transaction->commit()) {
		safs::log_err("Failed to remove edge: {} -> {}", parentId, edgeName);
	}
}
