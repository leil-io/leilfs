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
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include "kv/kv_utils.h"

namespace kv {

class IFuture;

/// Represents a key-value pair in the key-value store.
/// Keys and values are stored as vectors of bytes.
struct KeyValuePair {
	Key key;
	Value value;
};

/// Represents a key selector for range queries.
class KeySelector {
public:
	KeySelector(const Key &key, bool inclusive, int offset)
	    : key_(key), inclusive_(inclusive), offset_(offset) {}

	/// Returns the key for this selector.
	const Key &getKey() const { return key_; }

	/// Returns whether the key is inclusive in the range.
	bool isInclusive() const { return inclusive_; }

	/// Returns the offset for this selector.
	int getOffset() const { return offset_; }

private:
	Key key_;         ///< The key for this selector.
	bool inclusive_;  ///< Whether the key is inclusive in the range.
	int offset_;      ///< Offset for the key selector, used for pagination in range queries.
};

/// Result of a range query with information if there are more results available.
class GetRangeResult {
public:
	GetRangeResult(std::vector<KeyValuePair> pairs, bool hasMore)
	    : pairs_(std::move(pairs)), hasMore_(hasMore) {}

	/// Returns the key-value pairs in the result.
	const std::vector<KeyValuePair> &getPairs() const { return pairs_; }

	/// Returns whether there are more results available.
	bool hasMore() const { return hasMore_; }

private:
	std::vector<KeyValuePair> pairs_;  ///< The key-value pairs retrieved in the range query.
	bool hasMore_{false};              ///< True if more results are available beyond this range.
};

constexpr int kDefaultGetRangeLimit = 1000;

/// Interface for read-only transactions in the key-value store.
/// Provides methods to retrieve values and ranges of keys.
class IReadOnlyTransaction {
public:
	virtual ~IReadOnlyTransaction() = default;

	// Non-copyable, non-movable
	IReadOnlyTransaction(const IReadOnlyTransaction &) = delete;
	IReadOnlyTransaction &operator=(const IReadOnlyTransaction &) = delete;
	IReadOnlyTransaction(IReadOnlyTransaction &&) = delete;
	IReadOnlyTransaction &operator=(IReadOnlyTransaction &&) = delete;

	/// Retrieves the value for a given key.
	/// @param key The key to retrieve the value for.
	virtual std::optional<Value> get(const Key &key) = 0;

	/// Retrieves the value for a given key without adding it to the
	/// transaction's read conflict range (snapshot/advisory read).
	/// Use only when the read is advisory, for example, observing a
	/// counter before a blind atomic write, to avoid unnecessary conflicts.
	/// @warning Snapshot reads are not serializable and may observe stale
	///          or internally inconsistent data relative to other reads in
	///          the same transaction. Do not use for reads that must be
	///          transactionally consistent; use get() instead.
	/// @param key The key to retrieve the value for.
	virtual std::optional<Value> getSnapshot(const Key &key) = 0;

	/// Retrieves the value for a given key asynchronously.
	/// @param key The key to retrieve the value for.
	/// @return A future that will contain the value when ready.
	/// @note The transaction must remain alive until the future's get() method is called.
	virtual std::unique_ptr<IFuture> getAsync(const Key &key) = 0;

	/// Retrieves a range of keys and values
	/// @param start The starting key for the range.
	/// @param end The ending key for the range.
	/// @param limit The maximum number of key-value pairs to retrieve.
	virtual GetRangeResult getRange(const KeySelector &start, const KeySelector &end,
	                                int limit = kDefaultGetRangeLimit) = 0;

protected:
	IReadOnlyTransaction() = default;
};

/// Interface for read-write transactions in the key-value store.
/// Inherits from IReadOnlyTransaction and adds methods for modifying the database.
class IReadWriteTransaction : public IReadOnlyTransaction {
public:
	virtual ~IReadWriteTransaction() override = default;

	// Non-copyable, non-movable (also removes the clang-tidy warning)
	IReadWriteTransaction(const IReadWriteTransaction &) = delete;
	IReadWriteTransaction &operator=(const IReadWriteTransaction &) = delete;
	IReadWriteTransaction(IReadWriteTransaction &&) = delete;
	IReadWriteTransaction &operator=(IReadWriteTransaction &&) = delete;

	/// Sets a value for a given key.
	/// @param key The key to set the value for.
	/// @param value The value to set for the key.
	virtual void set(const Key &key, const Value &value) = 0;

	/// Atomically adds a delta value to the existing value for a given key.
	/// @param key The key to add the delta to.
	/// @param delta The delta value to add (must be little-endian).
	virtual void atomicAdd(const Key &key, const Value &delta) = 0;

	/// Removes a key from the database.
	/// @param key The key to remove.
	virtual void remove(const Key &key) = 0;

	/// Removes a half-open key range [start, end) from the database.
	/// @param start Inclusive start key.
	/// @param end Exclusive end key.
	virtual void removeRange(const Key &start, const Key &end) = 0;

	/// Commits the transaction, making all changes permanent.
	virtual bool commit() = 0;

	/// Returns the committed version of the transaction, if available.
	virtual std::optional<int64_t> getCommittedVersion() const = 0;

protected:
	IReadWriteTransaction() = default;
};

}  // namespace kv
