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

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "config/cfg.h"
#include "unittests/TemporaryDirectory.h"

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

// Calls cfg_term() even after a failed assertion.
struct CfgTermGuard {
	~CfgTermGuard() { cfg_term(); }
};

TEST(CfgTests, ReloadAfterChangingDirectory) {
	TemporaryDirectory dir("/tmp", "cfg-relative");
	const CfgTermGuard guard;
	const auto previous = std::filesystem::current_path();
	std::filesystem::current_path(dir.name());
	std::ofstream(dir.name() + "/test.cfg") << "LABEL = first\n";
	const int loaded = cfg_load("test.cfg", 0);

	// A daemon changes to its data directory after loading.
	std::filesystem::current_path(previous);
	ASSERT_EQ(loaded, 0);
	std::ofstream(dir.name() + "/test.cfg") << "LABEL = second\n";
	EXPECT_EQ(cfg_reload(), 0);
	EXPECT_EQ(cfg_getstring("LABEL", "none"), "second");
}

TEST(CfgTests, FailedReloadKeepsConfiguration) {
	TemporaryDirectory dir("/tmp", "cfg-failed-reload");
	const CfgTermGuard guard;
	const std::string path = dir.name() + "/test.cfg";
	std::ofstream(path) << "LABEL = first\n";
	ASSERT_EQ(cfg_load(path.c_str(), 0), 0);

	std::filesystem::remove(path);
	EXPECT_NE(cfg_reload(), 0);
	EXPECT_EQ(cfg_getstring("LABEL", "none"), "first");

	// A file that opens but cannot be read.
	std::filesystem::create_directory(path);
	EXPECT_NE(cfg_reload(), 0);
	EXPECT_EQ(cfg_getstring("LABEL", "none"), "first");
}
