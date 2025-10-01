#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fdb::utils {

// 8 bytes
constexpr int BE64_SIZE = sizeof(uint64_t);

inline std::array<uint8_t, BE64_SIZE> encodeBE64(uint64_t value) {
	value = std::byteswap(value);
	return std::bit_cast<std::array<uint8_t, BE64_SIZE>>(value);
}

inline uint64_t decodeBE64(const std::string_view stringView) {
	if (stringView.size() != BE64_SIZE) {
		throw std::invalid_argument("decodeBE64 expects 8 bytes");
	}
	uint64_t result{};
	std::memcpy(&result, stringView.data(), sizeof(result));
	return std::byteswap(result);
}

// Helpers to build string keys directly
inline std::string makeKey(const uint8_t prefix, uint64_t keyId) {
	keyId = std::byteswap(keyId);
	std::string key;
	key.reserve(1 + BE64_SIZE);
	key.push_back(static_cast<char>(prefix));

	// Append bytes directly from the swapped integer
	const auto *bytes = reinterpret_cast<const char *>(&keyId);  // NOLINT
	key.append(bytes, BE64_SIZE);
	return key;
}

inline uint64_t parseKeyId(const std::string_view key) {
	if (key.size() < 1 + BE64_SIZE) { throw std::invalid_argument("parseKeyId: key too short"); }
	return decodeBE64(key.substr(1, BE64_SIZE));
}

inline std::string composeKey(std::span<const uint8_t> prefixComponents, uint64_t keyId) {
	keyId = std::byteswap(keyId);
	std::string key;
	key.reserve(prefixComponents.size() + BE64_SIZE);
	for (const auto &prefix : prefixComponents) { key.push_back(static_cast<char>(prefix)); }
	// Append bytes directly from the swapped integer
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
	const auto *bytes = reinterpret_cast<const char *>(&keyId);
	key.append(bytes, BE64_SIZE);
	return key;
}

inline std::string composeKey(const std::initializer_list<uint8_t> prefix, const uint64_t keyId) {
	return composeKey(std::span<const uint8_t>(prefix.begin(), prefix.size()), keyId);
}

inline std::string encodePrefix(std::span<const uint8_t> prefixComponents) {
	std::string encoded;
	encoded.reserve(prefixComponents.size());
	for (const auto prefix : prefixComponents) { encoded.push_back(static_cast<char>(prefix)); }
	return encoded;
}

inline std::string encodePrefix(const std::initializer_list<uint8_t> prefixComponents) {
	return encodePrefix(
	    std::span<const uint8_t>(prefixComponents.begin(), prefixComponents.size()));
}

inline std::pair<std::string, std::string> makePrefixRange(const std::string &prefix) {
	std::string begin = prefix;
	std::string end = begin;
	end.push_back('\xFF');
	return {std::move(begin), std::move(end)};
}

inline std::pair<std::string, std::string> makePrefixRange(
    const std::span<const uint8_t> prefixComponents) {
	return makePrefixRange(encodePrefix(prefixComponents));
}

inline std::pair<std::string, std::string> makePrefixRange(
    const std::initializer_list<uint8_t> prefixComponents) {
	return makePrefixRange(
	    std::span<const uint8_t>(prefixComponents.begin(), prefixComponents.size()));
}

}  // namespace fdb::utils
