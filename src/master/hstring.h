#pragma once

#include "common/platform.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

#include "common/datapack.h"

/*! \brief String class keeping an additional field - precomputed hash.
 *
 *  Hash field is kept in order to speed up String comparison,
 *  which is a dominating operation in lookup (which is, recursively,
 *  dominating operation in file system).
 */
class HString : public ::std::string {
public:
	using HashType = uint32_t;

	HString() : ::std::string() {
		computeHash();
	}

	explicit HString(const char *str) : ::std::string(str) {
		computeHash();
	}

	explicit HString(const ::std::string &str) : ::std::string(str) {
		computeHash();
	}

	explicit HString(const char* str, std::size_t length) : ::std::string(str, length) {
		computeHash();
	}

	template<class InputIterator>
	explicit HString(InputIterator first, InputIterator last) : ::std::string(first, last) {
		computeHash();
	}

	HString(const HString &str) : ::std::string(str) {
		computeHash();
	}

	HString(HString &&str) noexcept : ::std::string(std::move(str)) {
		computeHash();
	}

	HString &operator=(const HString &str) {
		::std::string::operator=(str);
		computeHash();
		return *this;
	}

	HString &operator=(HString &&str) noexcept {
		::std::string::operator=(std::move(str));
		computeHash();
		return *this;
	}

	HString &operator=(const ::std::string &str) {
		::std::string::operator=(str);
		computeHash();
		return *this;
	}

	HashType hash() const {
		return hash_;
	}

	static HString hstringToLowerCase(const HString &input) {
		std::string str = input.c_str();
		std::transform(str.begin(), str.end(), str.begin(), ::tolower);
		return HString(str);
	}

	/// Serializes the length as uint32_t and then the string without adding extra null terminator
	static void serialize(uint8_t **destination, const HString &hstring) {
		put32bit(destination, uint32_t(hstring.length()));
		::memcpy(*destination, hstring.data(), hstring.length());
		*destination += hstring.length();
	}

	/// Deserializes the length as uint32_t and then the string
	static void deserialize(const uint8_t **source, HString &hstring) {
		uint32_t length{};
		get32bit(source, length);
		hstring.resize(length);
		::memcpy(hstring.data(), *source, length);
		*source += length;

		hstring.computeHash();
	}

private:
	void computeHash() {
		hash_ = ::std::hash< ::std::string>()(*static_cast< ::std::string *>(this));
	}

	HashType hash_;
};

inline bool operator==(const HString &str, const HString &other) {
	if (str.hash() == other.hash()) {
		return static_cast< ::std::string>(str) == static_cast< ::std::string>(other);
	}
	return false;
}

inline bool operator!=(const HString &str, const HString &other) {
	if (str.hash() == other.hash()) {
		return static_cast< ::std::string>(str) != static_cast< ::std::string>(other);
	}
	return true;
}
