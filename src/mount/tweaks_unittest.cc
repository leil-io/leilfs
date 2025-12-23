/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"
#include "mount/tweaks.h"

#include <gtest/gtest.h>

TEST(TweaksTests, GetAllValues) {
	std::atomic<uint32_t> u32(9);
	std::atomic<uint64_t> u64(19);
	std::atomic<bool> b(false);

	Tweaks t;
	t.registerVariable("u32", u32);
	t.registerVariable("u64", u64);
	t.registerVariable("bool", b);

	ASSERT_EQ("u32\t9\nu64\t19\nbool\tfalse\n", t.getAllValues());
}

TEST(TweaksTests, SetValue) {
	std::atomic<uint32_t> u32(9);
	std::atomic<uint64_t> u64(19);
	std::atomic<bool> b(false);

	Tweaks t;
	t.registerVariable("u32", u32);
	t.registerVariable("u64", u64);
	t.registerVariable("bool", b);

	t.setValue("u32", "   blah");
	t.setValue("u32", "blah");
	t.setValue("u32", "");
	t.setValue("u32", "\n");
	ASSERT_EQ(9U, u32);

	t.setValue("u32", "  16 xxx");
	ASSERT_EQ(16U, u32);

	t.setValue("u32", "15\n");
	ASSERT_EQ(15U, u32);

	t.setValue("u64", "150");
	ASSERT_EQ(15U, u32);
	ASSERT_EQ(150U, u64);
	ASSERT_FALSE(b.load());

	t.setValue("bool", "true");
	ASSERT_TRUE(b.load());
	t.setValue("bool", "false");
	ASSERT_FALSE(b.load());
	t.setValue("bool", "true\n");
	ASSERT_TRUE(b.load());
}

TEST(TweaksTests, StringVariableBasicUsage) {
	std::string s = "initial";
	std::mutex sMutex;

	Tweaks t;
	t.registerVariable("str", s, sMutex);

	ASSERT_EQ("str\tinitial\n", t.getAllValues());
	ASSERT_EQ("initial", t.getValue("str"));

	t.setValue("str", "updated");

	ASSERT_EQ("updated", t.getValue("str"));

	{
		std::lock_guard<std::mutex> lock(sMutex);
		ASSERT_EQ("updated", s);
	}
}

TEST(TweaksTests, MixedTypesIncludingString) {
	std::atomic<uint32_t> u32(1);
	std::atomic<bool> b(false);
	std::string s = "foo";
	std::mutex sMutex;

	Tweaks t;
	t.registerVariable("u32", u32);
	t.registerVariable("bool", b);
	t.registerVariable("str", s, sMutex);

	t.setValue("u32", "42");
	t.setValue("bool", "true");
	t.setValue("str", "bar");

	ASSERT_EQ(42U, u32.load());
	ASSERT_TRUE(b.load());

	{
		std::lock_guard<std::mutex> lock(sMutex);
		ASSERT_EQ("bar", s);
	}

	ASSERT_EQ("bar", t.getValue("str"));
}
