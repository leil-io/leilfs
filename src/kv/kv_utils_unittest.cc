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

#include "kv/kv_utils.h"

#include <gtest/gtest.h>

TEST(KVUtilsTest, ToBytes) {
	const char *text = "Hello, World!";
	std::string textString{text};
	std::string_view textStringView{text, strlen(text)};

	// Create expected result as a kv::Value (std::vector<uint8_t>)
	kv::Value expected(strlen(text));
	std::memcpy(expected.data(), text, strlen(text));

	// Passing a plain char *
	auto result = kv::toBytes(text);
	EXPECT_EQ(result, expected);

	// Passing a std::string
	result = kv::toBytes(textString);
	EXPECT_EQ(result, expected);

	// Passing a std::string_view
	result = kv::toBytes(textStringView);
	EXPECT_EQ(result, expected);
}

TEST(KVUtilsTest, PrefixEnd) {
	// Normal case: last byte incremented, length preserved
	{
		kv::Key prefix = {0x01, 0x02};
		kv::Key expected = {0x01, 0x03};
		EXPECT_EQ(kv::prefixEnd(prefix), expected);
	}

	// Last byte is 0xFF: carry into previous byte and truncate
	{
		kv::Key prefix = {0x01, 0xFF};
		kv::Key expected = {0x02};
		EXPECT_EQ(kv::prefixEnd(prefix), expected);
	}

	// Multiple trailing 0xFF bytes: carry and truncate all of them
	{
		kv::Key prefix = {0x01, 0xFF, 0xFF};
		kv::Key expected = {0x02};
		EXPECT_EQ(kv::prefixEnd(prefix), expected);
	}

	// Single byte, not 0xFF: just increment
	{
		kv::Key prefix = {0x42};
		kv::Key expected = {0x43};
		EXPECT_EQ(kv::prefixEnd(prefix), expected);
	}

	// All 0xFF bytes: no finite upper bound, must throw
	{
		kv::Key prefix = {0xFF, 0xFF};
		EXPECT_THROW(kv::prefixEnd(prefix), std::invalid_argument);
	}

	// Single 0xFF byte: no finite upper bound, must throw
	{
		kv::Key prefix = {0xFF};
		EXPECT_THROW(kv::prefixEnd(prefix), std::invalid_argument);
	}

	// Empty prefix: no finite upper bound, must throw
	{
		kv::Key prefix = {};
		EXPECT_THROW(kv::prefixEnd(prefix), std::invalid_argument);
	}
}

TEST(KVUtilsTest, ToBytesLE) {
	// Test uint8_t (single byte - endianness doesn't matter)
	{
		constexpr uint8_t kTestValue = 0x42;
		kv::Value expected = {kTestValue};
		auto result = kv::toBytesLE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test uint16_t - least significant byte first
	{
		constexpr uint16_t kTestValue = 0x1234;
		constexpr uint8_t kLowByte = 0x34;
		constexpr uint8_t kHighByte = 0x12;
		kv::Value expected = {kLowByte, kHighByte};
		auto result = kv::toBytesLE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test uint32_t in little-endian byte order
	{
		constexpr uint32_t kTestValue = 0x12345678;
		constexpr uint8_t kByte0 = 0x78;
		constexpr uint8_t kByte1 = 0x56;
		constexpr uint8_t kByte2 = 0x34;
		constexpr uint8_t kByte3 = 0x12;
		kv::Value expected = {kByte0, kByte1, kByte2, kByte3};
		auto result = kv::toBytesLE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test uint64_t in little-endian byte order
	{
		constexpr uint64_t kTestValue = 0x123456789ABCDEF0;
		constexpr uint8_t kByte0 = 0xF0;
		constexpr uint8_t kByte1 = 0xDE;
		constexpr uint8_t kByte2 = 0xBC;
		constexpr uint8_t kByte3 = 0x9A;
		constexpr uint8_t kByte4 = 0x78;
		constexpr uint8_t kByte5 = 0x56;
		constexpr uint8_t kByte6 = 0x34;
		constexpr uint8_t kByte7 = 0x12;
		kv::Value expected = {kByte0, kByte1, kByte2, kByte3, kByte4, kByte5, kByte6, kByte7};
		auto result = kv::toBytesLE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test negative int16_t (two's complement representation)
	{
		constexpr int16_t kTestValue = -1;  // 0xFFFF in two's complement
		constexpr uint8_t kByte = 0xFF;
		kv::Value expected = {kByte, kByte};
		auto result = kv::toBytesLE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test negative int32_t with specific bit pattern
	{
		constexpr int32_t kTestValue = -256;  // 0xFFFFFF00 in two's complement
		constexpr uint8_t kLowByte = 0x00;
		constexpr uint8_t kHighByte = 0xFF;
		kv::Value expected = {kLowByte, kHighByte, kHighByte, kHighByte};
		auto result = kv::toBytesLE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test zero value
	{
		constexpr uint32_t kTestValue = 0;
		constexpr uint8_t kZeroByte = 0x00;
		kv::Value expected = {kZeroByte, kZeroByte, kZeroByte, kZeroByte};
		auto result = kv::toBytesLE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test maximum uint16_t value
	{
		constexpr uint16_t kTestValue = 0xFFFF;
		constexpr uint8_t kMaxByte = 0xFF;
		kv::Value expected = {kMaxByte, kMaxByte};
		auto result = kv::toBytesLE(kTestValue);
		EXPECT_EQ(result, expected);
	}
}

TEST(KVUtilsTest, ToBytesBE) {
	// Test uint8_t (single byte - endianness doesn't matter)
	{
		constexpr uint8_t kTestValue = 0x42;
		kv::Value expected = {kTestValue};
		auto result = kv::toBytesBE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test uint16_t - most significant byte first
	{
		constexpr uint16_t kTestValue = 0x1234;
		constexpr uint8_t kHighByte = 0x12;
		constexpr uint8_t kLowByte = 0x34;
		kv::Value expected = {kHighByte, kLowByte};
		auto result = kv::toBytesBE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test uint32_t in big-endian byte order
	{
		constexpr uint32_t kTestValue = 0x12345678;
		constexpr uint8_t kByte0 = 0x12;
		constexpr uint8_t kByte1 = 0x34;
		constexpr uint8_t kByte2 = 0x56;
		constexpr uint8_t kByte3 = 0x78;
		kv::Value expected = {kByte0, kByte1, kByte2, kByte3};
		auto result = kv::toBytesBE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test uint64_t in big-endian byte order
	{
		constexpr uint64_t kTestValue = 0x123456789ABCDEF0;
		constexpr uint8_t kByte0 = 0x12;
		constexpr uint8_t kByte1 = 0x34;
		constexpr uint8_t kByte2 = 0x56;
		constexpr uint8_t kByte3 = 0x78;
		constexpr uint8_t kByte4 = 0x9A;
		constexpr uint8_t kByte5 = 0xBC;
		constexpr uint8_t kByte6 = 0xDE;
		constexpr uint8_t kByte7 = 0xF0;
		kv::Value expected = {kByte0, kByte1, kByte2, kByte3, kByte4, kByte5, kByte6, kByte7};
		auto result = kv::toBytesBE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test negative int16_t (two's complement representation)
	{
		constexpr int16_t kTestValue = -1;  // 0xFFFF in two's complement
		constexpr uint8_t kByte = 0xFF;
		kv::Value expected = {kByte, kByte};
		auto result = kv::toBytesBE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test negative int32_t with specific bit pattern
	{
		constexpr int32_t kTestValue = -256;  // 0xFFFFFF00 in two's complement
		constexpr uint8_t kHighByte = 0xFF;
		constexpr uint8_t kLowByte = 0x00;
		kv::Value expected = {kHighByte, kHighByte, kHighByte, kLowByte};
		auto result = kv::toBytesBE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test zero value
	{
		constexpr uint32_t kTestValue = 0;
		constexpr uint8_t kZeroByte = 0x00;
		kv::Value expected = {kZeroByte, kZeroByte, kZeroByte, kZeroByte};
		auto result = kv::toBytesBE(kTestValue);
		EXPECT_EQ(result, expected);
	}

	// Test maximum uint16_t value
	{
		constexpr uint16_t kTestValue = 0xFFFF;
		constexpr uint8_t kMaxByte = 0xFF;
		kv::Value expected = {kMaxByte, kMaxByte};
		auto result = kv::toBytesBE(kTestValue);
		EXPECT_EQ(result, expected);
	}
}

TEST(KVUtilsTest, FromBytesLE) {
	// Test uint8_t (single byte)
	{
		constexpr uint8_t kExpected = 0x42;
		kv::Value value = {kExpected};
		auto result = kv::fromBytesLE<uint8_t>(value);
		EXPECT_EQ(result, kExpected);
	}

	// Test uint16_t from little-endian bytes
	{
		constexpr uint16_t kExpected = 0x1234;
		constexpr uint8_t kLowByte = 0x34;
		constexpr uint8_t kHighByte = 0x12;
		kv::Value value = {kLowByte, kHighByte};
		auto result = kv::fromBytesLE<uint16_t>(value);
		EXPECT_EQ(result, kExpected);
	}

	// Test uint32_t from little-endian bytes
	{
		constexpr uint32_t kExpected = 0x12345678;
		constexpr uint8_t kByte0 = 0x78;
		constexpr uint8_t kByte1 = 0x56;
		constexpr uint8_t kByte2 = 0x34;
		constexpr uint8_t kByte3 = 0x12;
		kv::Value value = {kByte0, kByte1, kByte2, kByte3};
		auto result = kv::fromBytesLE<uint32_t>(value);
		EXPECT_EQ(result, kExpected);
	}

	// Test uint64_t from little-endian bytes
	{
		constexpr uint64_t kExpected = 0x123456789ABCDEF0;
		constexpr uint8_t kByte0 = 0xF0;
		constexpr uint8_t kByte1 = 0xDE;
		constexpr uint8_t kByte2 = 0xBC;
		constexpr uint8_t kByte3 = 0x9A;
		constexpr uint8_t kByte4 = 0x78;
		constexpr uint8_t kByte5 = 0x56;
		constexpr uint8_t kByte6 = 0x34;
		constexpr uint8_t kByte7 = 0x12;
		kv::Value value = {kByte0, kByte1, kByte2, kByte3, kByte4, kByte5, kByte6, kByte7};
		auto result = kv::fromBytesLE<uint64_t>(value);
		EXPECT_EQ(result, kExpected);
	}

	// Test partial bytes (smaller vector than type size)
	{
		constexpr uint32_t kExpected = 0x1234;
		constexpr uint8_t kByte0 = 0x34;
		constexpr uint8_t kByte1 = 0x12;
		kv::Value value = {kByte0, kByte1};  // Only 2 bytes for uint32_t
		auto result = kv::fromBytesLE<uint32_t>(value);
		EXPECT_EQ(result, kExpected);
	}

	// Test zero value
	{
		constexpr uint32_t kExpected = 0;
		constexpr uint8_t kZeroByte = 0x00;
		kv::Value value = {kZeroByte, kZeroByte, kZeroByte, kZeroByte};
		auto result = kv::fromBytesLE<uint32_t>(value);
		EXPECT_EQ(result, kExpected);
	}

	// Test maximum uint16_t value
	{
		constexpr uint16_t kExpected = 0xFFFF;
		constexpr uint8_t kMaxByte = 0xFF;
		kv::Value value = {kMaxByte, kMaxByte};
		auto result = kv::fromBytesLE<uint16_t>(value);
		EXPECT_EQ(result, kExpected);
	}

	// Test exception when value size exceeds type size
	{
		constexpr uint8_t kByte = 0x12;
		kv::Value value = {kByte, kByte, kByte};  // 3 bytes for uint16_t (2 bytes)
		EXPECT_THROW(kv::fromBytesLE<uint16_t>(value), std::invalid_argument);
	}
}

TEST(KVUtilsTest, EncodeKeyBE) {
	// Test with prefix and single uint32_t value (like NODE_)
	{
		constexpr std::string_view kPrefix = "N";
		constexpr uint32_t kInodeId = 0x12345678;
		// Create expected manually: prefix + big-endian bytes
		kv::Key expected;
		expected.push_back('N');
		expected.push_back(0x12);
		expected.push_back(0x34);
		expected.push_back(0x56);
		expected.push_back(0x78);

		auto result = kv::encodeKeyBE(kPrefix, kInodeId);
		EXPECT_EQ(result, expected);
	}

	// Test with prefix and two uint32_t values (like EDGE_ parent, child)
	{
		constexpr std::string_view kPrefix = "E";
		constexpr uint32_t kParentId = 0x00000001;
		constexpr uint32_t kChildId = 0x00000002;

		kv::Key expected;
		expected.push_back('E');
		// Parent ID in big-endian
		expected.push_back(0x00);
		expected.push_back(0x00);
		expected.push_back(0x00);
		expected.push_back(0x01);
		// Child ID in big-endian
		expected.push_back(0x00);
		expected.push_back(0x00);
		expected.push_back(0x00);
		expected.push_back(0x02);

		auto result = kv::encodeKeyBE(kPrefix, kParentId, kChildId);
		EXPECT_EQ(result, expected);
	}

	// Test with uint64_t value
	{
		constexpr std::string_view kPrefix = "D";
		constexpr uint64_t kValue = 0x123456789ABCDEF0;

		kv::Key expected;
		expected.push_back('D');
		expected.push_back(0x12);
		expected.push_back(0x34);
		expected.push_back(0x56);
		expected.push_back(0x78);
		expected.push_back(0x9A);
		expected.push_back(0xBC);
		expected.push_back(0xDE);
		expected.push_back(0xF0);

		auto result = kv::encodeKeyBE(kPrefix, kValue);
		EXPECT_EQ(result, expected);
	}

	// Test with mixed types (uint16_t, uint32_t)
	{
		constexpr std::string_view kPrefix = "M";
		constexpr uint16_t kShort = 0xABCD;
		constexpr uint32_t kInt = 0x12345678;

		kv::Key expected;
		expected.push_back('M');
		// uint16_t in big-endian
		expected.push_back(0xAB);
		expected.push_back(0xCD);
		// uint32_t in big-endian
		expected.push_back(0x12);
		expected.push_back(0x34);
		expected.push_back(0x56);
		expected.push_back(0x78);

		auto result = kv::encodeKeyBE(kPrefix, kShort, kInt);
		EXPECT_EQ(result, expected);
	}

	// Test with uint8_t value
	{
		constexpr std::string_view kPrefix = "I";
		constexpr uint8_t kValue = 0x42;

		kv::Key expected;
		expected.push_back('I');
		expected.push_back(kValue);

		auto result = kv::encodeKeyBE(kPrefix, kValue);
		EXPECT_EQ(result, expected);
	}

	// Test with empty prefix
	{
		constexpr std::string_view kPrefix = "";
		constexpr uint32_t kValue = 0x12345678;

		kv::Key expected;
		expected.push_back(0x12);
		expected.push_back(0x34);
		expected.push_back(0x56);
		expected.push_back(0x78);

		auto result = kv::encodeKeyBE(kPrefix, kValue);
		EXPECT_EQ(result, expected);
	}

	// Test with three values (uint16_t, uint32_t, uint64_t)
	{
		constexpr std::string_view kPrefix = "K";
		constexpr uint16_t kVal1 = 0x0001;
		constexpr uint32_t kVal2 = 0x00000002;
		constexpr uint64_t kVal3 = 0x0000000000000003;

		kv::Key expected;
		expected.push_back('K');
		// uint16_t
		expected.push_back(0x00);
		expected.push_back(0x01);
		// uint32_t
		expected.push_back(0x00);
		expected.push_back(0x00);
		expected.push_back(0x00);
		expected.push_back(0x02);
		// uint64_t
		expected.push_back(0x00);
		expected.push_back(0x00);
		expected.push_back(0x00);
		expected.push_back(0x00);
		expected.push_back(0x00);
		expected.push_back(0x00);
		expected.push_back(0x00);
		expected.push_back(0x03);

		auto result = kv::encodeKeyBE(kPrefix, kVal1, kVal2, kVal3);
		EXPECT_EQ(result, expected);
	}

	// Test correct size calculation
	{
		constexpr std::string_view kPrefix = "TEST_";
		constexpr uint32_t kValue = 0x12345678;

		auto result = kv::encodeKeyBE(kPrefix, kValue);
		EXPECT_EQ(result.size(), kPrefix.size() + sizeof(kValue));
	}
}
