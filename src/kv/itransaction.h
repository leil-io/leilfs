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
#include <optional>
#include <stdexcept>
#include <vector>

namespace kv {

using Key = std::vector<uint8_t>;
using Value = std::vector<uint8_t>;

/// Converts string, string_view and const char* to a vector of uint8_t.
inline std::vector<uint8_t> toU8Vector(std::string_view str) { return {str.begin(), str.end()}; }

/// Converts integral types to a vector of uint8_t.
template <typename T>
	requires(std::is_integral_v<T>)
inline std::vector<uint8_t> toU8VectorLittleEndian(T value) {
	std::vector<uint8_t> bytes(sizeof(value));
	for (size_t i = 0; i < sizeof(T); ++i) {
		bytes[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
	}
	return bytes;
}

template <typename T>
    requires(std::is_integral_v<T>)
T fromU8VectorLittleEndianToInt(const Value &value) {
	if (value.size() > sizeof(T)) { throw std::invalid_argument("Invalid value size"); }
	T result = 0;
	for (size_t i = 0; i < value.size(); ++i) { result |= static_cast<T>(value[i]) << (8 * i); }
	return result;
}

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

	/// Retrieves the value for a given key.
	/// @param key The key to retrieve the value for.
	virtual std::optional<Value> get(const Key &key) = 0;

	/// Retrieves a range of keys and values
	/// @param start The starting key for the range.
	/// @param end The ending key for the range.
	/// @param limit The maximum number of key-value pairs to retrieve.
	virtual GetRangeResult getRange(const KeySelector &start, const KeySelector &end,
	                                int limit = kDefaultGetRangeLimit) = 0;
};

/// Interface for read-write transactions in the key-value store.
/// Inherits from IReadOnlyTransaction and adds methods for modifying the database.
class IReadWriteTransaction : public IReadOnlyTransaction {
public:
	virtual ~IReadWriteTransaction() override = default;

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

	/// Commits the transaction, making all changes permanent.
	virtual bool commit() = 0;

	/// Returns the committed version of the transaction, if available.
	virtual std::optional<int64_t> getCommittedVersion() const = 0;
};

}  // namespace kv
