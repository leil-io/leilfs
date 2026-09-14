/*
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

#include <gtest/gtest.h>

#include "config/cfg.h"

TEST(CfgTests, CfgParseSize) {
	// Bytes
	EXPECT_EQ(0, cfg_parse_size("0"));
	EXPECT_EQ(100, cfg_parse_size("100"));
	EXPECT_EQ(1, cfg_parse_size("1"));
	EXPECT_EQ(1, cfg_parse_size("1b"));
	EXPECT_EQ(1, cfg_parse_size("1B"));

	// Kilo
	EXPECT_EQ(1000, cfg_parse_size("1k"));
	EXPECT_EQ(1000, cfg_parse_size("1K"));
	EXPECT_EQ(1024, cfg_parse_size("1Ki"));
	EXPECT_EQ(1024, cfg_parse_size("1kI"));
	EXPECT_EQ(1024, cfg_parse_size("1ki"));
	EXPECT_EQ(1000, cfg_parse_size("1KB"));
	EXPECT_EQ(1000, cfg_parse_size("1kb"));
	EXPECT_EQ(1000, cfg_parse_size("1Kb"));
	EXPECT_EQ(1000, cfg_parse_size("1kB"));
	EXPECT_EQ(1024, cfg_parse_size("1kib"));
	EXPECT_EQ(1024, cfg_parse_size("1KiB"));
	EXPECT_EQ(1024, cfg_parse_size("1kIB"));
	EXPECT_EQ(1024, cfg_parse_size("1kiB"));

	// Giga
	EXPECT_EQ(1000000000, cfg_parse_size("1G"));
	EXPECT_EQ(1000000000, cfg_parse_size("1GB"));
	EXPECT_EQ(1073741824, cfg_parse_size("1Gi"));
	EXPECT_EQ(1073741824, cfg_parse_size("1GiB"));

	// With spaces
	EXPECT_EQ(1024, cfg_parse_size("1 KiB"));
	EXPECT_EQ(1024, cfg_parse_size("1KiB "));
	EXPECT_EQ(1024, cfg_parse_size(" 1 KiB "));

	// With dots
	EXPECT_EQ(1024, cfg_parse_size("1.KiB"));
	EXPECT_EQ(512, cfg_parse_size(".5 KiB"));
	EXPECT_EQ(512, cfg_parse_size("0.5 KiB"));
	EXPECT_EQ(1500, cfg_parse_size("1.5K"));

	// Invalid values or suffixes
	EXPECT_EQ(-1, cfg_parse_size(""));
	EXPECT_EQ(-1, cfg_parse_size("1x"));
	EXPECT_EQ(-1, cfg_parse_size("1Bx"));
	EXPECT_EQ(-1, cfg_parse_size("1 KBx"));
	EXPECT_EQ(-1, cfg_parse_size("1_KiB"));
}
