/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"
#include "common/legacy_string.h"

#include <gtest/gtest.h>

#include "unittests/inout_pair.h"

TEST(LegacyStringTests, Serialization8Bit) {
	std::vector<uint8_t> buffer;
	SAUNAFS_DEFINE_INOUT_PAIR(LegacyString<uint8_t>,   string8, "", "");
	SAUNAFS_DEFINE_INOUT_PAIR(LegacyString<uint16_t>, string16, "", "");
	SAUNAFS_DEFINE_INOUT_PAIR(LegacyString<uint32_t>, string32, "", "");

	std::stringstream ss;
	for (int i = 0; i < 1000; ++i) {
		ss << "1=-09;'{}[]\\|/.,<>?!@#$%^&*()qwertsdfag d1426asdfghjklmn~!@#$%^&*() 63245634345347";
	}

	string8In = ss.str().substr(0, 200);
	buffer.clear();
	ASSERT_NO_THROW(serialize(buffer, string8In));
	EXPECT_EQ(string8In.length() + 1, buffer.size());
	ASSERT_NO_THROW(deserialize(buffer, string8Out));
	SAUNAFS_VERIFY_INOUT_PAIR(string8);

	string16In = ss.str().substr(0, 20000);
	buffer.clear();
	ASSERT_NO_THROW(serialize(buffer, string16In));
	EXPECT_EQ(string16In.length() + 2, buffer.size());
	ASSERT_NO_THROW(deserialize(buffer, string16Out));
	SAUNAFS_VERIFY_INOUT_PAIR(string16);

	string32In = ss.str().substr(0, 70000);
	buffer.clear();
	ASSERT_NO_THROW(serialize(buffer, string32In));
	EXPECT_EQ(string32In.length() + 4, buffer.size());
	ASSERT_NO_THROW(deserialize(buffer, string32Out));
	SAUNAFS_VERIFY_INOUT_PAIR(string32);
}

TEST(LegacyStringTests, MaxLength) {
	std::vector<uint8_t> buffer;
	uint32_t maxLength8 = LegacyString<uint8_t>::maxLength();
	ASSERT_ANY_THROW(serialize(buffer, LegacyString<uint8_t>(maxLength8 + 1, 'x')));
	ASSERT_TRUE(buffer.empty());
	ASSERT_NO_THROW(serialize(buffer, LegacyString<uint8_t>(maxLength8, 'x')));
	LegacyString<uint8_t> out;
	deserialize(buffer, out);
	EXPECT_EQ(LegacyString<uint8_t>(maxLength8, 'x'), out);
}
