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
#include <set>
#include <vector>

#include "common/datapack.h"
#include "kv/ikv_engine.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/kv_common_keys.h"
#include "master/metadata_backend_interface.h"
#include "slogger/slogger.h"

namespace checkpoints {

/// Serializes a list of checkpoint versions into a kv::Value for storage under
/// kMetaCheckpointVersionsKey.
///
/// The serialized form is a concatenation of 8-byte elements:
/// `<u64_0><u64_1>...<u64_n>`, where each element is written using datapack helpers
/// (`put64bit`), which use big-endian order.
///
/// @param versions Checkpoint versions to encode.
/// @return A kv::Value containing exactly `versions.size() * 8` bytes.
inline kv::Value serializeCheckpointVersions(const std::vector<uint64_t> &versions) {
	kv::Value value(versions.size() * sizeof(uint64_t));
	uint8_t *valuePtr = value.data();
	for (uint64_t version : versions) { put64bit(&valuePtr, version); }
	return value;
}

/// Deserializes a list of checkpoint versions from a kv::Value previously produced by
/// serializeCheckpointVersions().
///
/// The input is expected to be produced by serializeCheckpointVersions() (i.e. a concatenation of
/// 8-byte values written using `put64bit`).
///
/// @note If `value.size()` is not a multiple of 8, this is considered a malformed input and an
/// empty list will be returned (with a warning log).
/// @param value Encoded byte buffer.
/// @return Deserialized checkpoint versions in the same order they were encoded.
inline std::vector<uint64_t> deserializeCheckpointVersions(const kv::Value &value) {
	std::vector<uint64_t> versions;
	if (value.size() % sizeof(uint64_t) != 0) {
		safs::log_warn("Ignoring malformed checkpoint catalog with size {}", value.size());
		return versions;
	}

	const uint8_t *valuePtr = value.data();
	const uint8_t *end = valuePtr + value.size();
	while (valuePtr < end) { versions.push_back(get64bit(&valuePtr)); }

	return versions;
}

/// Loads the retained checkpoint version catalog from FDB.
///
/// Reads the value stored under kMetaCheckpointVersionsKey in a new read-only transaction
/// and deserializes it via deserializeCheckpointVersions(). Returns an empty list if the
/// key is absent, the engine pointer is null, or the stored value is malformed.
///
/// @param kvEngine KV engine to read from. May be null (returns empty list).
/// @return Ordered list of retained checkpoint versions, or empty on any error.
inline std::vector<uint64_t> loadCheckpointVersions(kv::IKVEngine *kvEngine) {
	std::vector<uint64_t> versions;
	if (kvEngine == nullptr) { return versions; }

	auto transaction = kvEngine->createReadOnlyTransaction();
	auto versionsValue = transaction->get(kv::toBytes(kMetaCheckpointVersionsKey));
	if (!versionsValue.has_value()) { return versions; }

	versions = deserializeCheckpointVersions(*versionsValue);

	return versions;
}

/// Persists the retained checkpoint version catalog to FDB inside an existing transaction.
///
/// Deduplicates and sorts `versions` in place, then serializes the result with
/// serializeCheckpointVersions() and writes it under kMetaCheckpointVersionsKey.
/// The caller is responsible for committing the transaction.
///
/// @note `versions` is mutated: duplicates are removed and elements are sorted ascending.
/// @note Returns kOpFailure without writing if `versions` is empty.
///
/// @param transaction Open read-write transaction to write into. Must not be null.
/// @param versions    Checkpoint versions to persist. Modified in place (dedup + sort).
/// @return kOpSuccess on success; kOpFailure if transaction is null or versions is empty.
inline int8_t saveCheckpointVersions(kv::IReadWriteTransaction *transaction,
                                     std::vector<uint64_t> &versions) {
	if (transaction == nullptr) { return kOpFailure; }

	if (versions.empty()) {
		safs::log_warn("{}: Checkpoint versions are empty, cannot save checkpoint versions",
		               __func__);
		return kOpFailure;
	}

	std::set<uint64_t> checkpointVersionsSet(versions.begin(), versions.end());

	// Set the final list of checkpoint versions vector and persist it in FDB
	versions.assign(checkpointVersionsSet.begin(), checkpointVersionsSet.end());
	auto checkpointVersionsValue = serializeCheckpointVersions(versions);
	transaction->set(kv::toBytes(kMetaCheckpointVersionsKey), checkpointVersionsValue);

	return kOpSuccess;
}

}  // namespace checkpoints
