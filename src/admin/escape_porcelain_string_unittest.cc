/*
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

#include "admin/escape_porcelain_string.h"
#include "common/platform.h"

#include <gtest/gtest.h>

TEST(EscapePorcelainStringTests, EscapePorcelainString) {
	EXPECT_EQ(R"("")", escapePorcelainString(R"()"));
	EXPECT_EQ(R"(lubie)", escapePorcelainString(R"(lubie)"));
	EXPECT_EQ(R"(1*_,2*hdd)", escapePorcelainString(R"(1*_,2*hdd)"));
	EXPECT_EQ(R"("\\")", escapePorcelainString(R"(\)"));
	EXPECT_EQ(R"("\"")", escapePorcelainString(R"(")"));
	EXPECT_EQ(R"("\"\"")", escapePorcelainString(R"("")"));
	EXPECT_EQ(R"("lubie placuszki")", escapePorcelainString(R"(lubie placuszki)"));
	EXPECT_EQ(R"("lubie\\placuszki")", escapePorcelainString(R"(lubie\placuszki)"));
	EXPECT_EQ(R"("lubie\\\\placuszki")", escapePorcelainString(R"(lubie\\placuszki)"));
	EXPECT_EQ(R"("lubie\\\\\\placuszki")", escapePorcelainString(R"(lubie\\\placuszki)"));
	EXPECT_EQ(R"("\\lubie\\placuszki\\")", escapePorcelainString(R"(\lubie\placuszki\)"));
	EXPECT_EQ(R"("\"lubie\\placuszki\"")", escapePorcelainString(R"("lubie\placuszki")"));
	EXPECT_EQ(R"("lubie\\ placuszki")", escapePorcelainString(R"(lubie\ placuszki)"));
	EXPECT_EQ(R"("\"lubie\\ placuszki\"")", escapePorcelainString(R"("lubie\ placuszki")"));
}
