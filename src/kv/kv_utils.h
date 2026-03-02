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

#pragma once

#include "common/platform.h"

#include <bit>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "kv/ifuture.h"
#include "kv/kv_types.h"

namespace kv {

/// Converts string, string_view and const char* to a vector of uint8_t.
inline Bytes toBytes(std::string_view str) { return {str.begin(), str.end()}; }

/// Appends a string or string_view to a vector of uint8_t.
inline void appendStr(Bytes &destination, std::string_view str) {
	destination.insert(destination.end(), str.begin(), str.end());
}

/// Returns the exclusive upper bound for a prefix range scan.
///
/// This is equivalent to FoundationDB's `strinc` helper: the smallest key strictly greater
/// than all keys that begin with @p prefix.
///
/// @throws std::invalid_argument if @p prefix is empty or all 0xFF bytes and has no finite upper
/// bound.
inline Key prefixEnd(Key prefix) {
	for (size_t i = prefix.size(); i > 0; --i) {
		if (prefix[i - 1] != 0xFF) {
			++prefix[i - 1];
			prefix.resize(i);
			return prefix;
		}
	}

	throw std::invalid_argument("Prefix has no finite upper bound");
}

/// Converts integral types to a vector of uint8_t encoded in little-endian order.
template <typename T>
    requires(std::is_integral_v<T>)
inline Bytes toBytesLE(T value) {
	if constexpr (std::endian::native == std::endian::big) { value = std::byteswap(value); }

	Bytes result(sizeof(T));
	std::memcpy(result.data(), &value, sizeof(T));
	return result;
}

/// Converts integral types to a vector of uint8_t encoded in big-endian order.
template <typename T>
    requires(std::is_integral_v<T>)
inline Bytes toBytesBE(T value) {
	if constexpr (std::endian::native == std::endian::little) { value = std::byteswap(value); }

	Bytes result(sizeof(T));
	std::memcpy(result.data(), &value, sizeof(T));
	return result;
}

template <typename T>
    requires(std::is_integral_v<T>)
T fromBytesLE(const Value &value) {
	if (value.size() > sizeof(T)) { throw std::invalid_argument("Invalid value size"); }
	T result = 0;
	for (size_t i = 0; i < value.size(); ++i) { result |= static_cast<T>(value[i]) << (8 * i); }
	return result;
}

/// Encodes a key with a prefix followed by one or more integral values in big-endian order.
/// @param prefix The prefix to prepend to the key.
/// @param values One or more integral values to encode in big-endian order.
/// @return A vector of uint8_t representing the encoded key.
template <typename... Args>
    requires(std::is_integral_v<Args> && ...)
Key encodeKeyBE(std::string_view prefix, Args... values) {
	// Calculate total size needed
	constexpr size_t totalSize = (sizeof(Args) + ...);
	Key key(prefix.size() + totalSize);

	// Copy prefix
	std::memcpy(key.data(), prefix.data(), prefix.size());

	// Helper to encode each value in big-endian and append to key
	size_t offset = prefix.size();
	auto encodeValue = [&key, &offset]<typename T>(T value) {
		if constexpr (std::endian::native == std::endian::little) { value = std::byteswap(value); }
		std::memcpy(key.data() + offset, &value, sizeof(T));
		offset += sizeof(T);
	};

	// Encode all values
	(encodeValue(values), ...);

	return key;
}

/// Extracts an integral value encoded in little-endian format from an async future result.
/// Throws std::runtime_error if the key is not found or an error occurs.
/// @param future The future containing the async get operation result.
/// @param key The key name used for error reporting.
/// @return The decoded integral value in native byte order.
template <typename T>
    requires(std::is_integral_v<T>)
T extractIntegralLEFromFuture(std::unique_ptr<kv::IFuture> &future, std::string_view key) {
	int error = 0;
	auto result = future->get(&error);

	if (error == 0 && result.has_value()) { return kv::fromBytesLE<T>(result.value()); }

	if (error == 0 && !result.has_value()) {
		std::ostringstream oss;
		oss << "Key not found in future result for key '" << key << "'";
		throw std::runtime_error(oss.str());
	}

	std::ostringstream oss;
	oss << "Failed to extract value from future for key '" << key << "': error code " << error;
	throw std::runtime_error(oss.str());
}

}  // namespace kv
