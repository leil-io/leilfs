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

#include <gtest/gtest.h>
#include <sstream>

#include "mount/io_limit_group.h"

TEST(IoLimitGroupTests, Empty) {
	std::stringstream input("");
	EXPECT_THROW(getIoLimitGroupId(input, "blkio"), GetIoLimitGroupIdException);
}

TEST(IoLimitGroupTests, NoMatching) {
	std::stringstream input("123:foo,bar:/test\n234:baz:/rmrfHOME\n");
	EXPECT_THROW(getIoLimitGroupId(input, "blkio"), GetIoLimitGroupIdException);
}

TEST(IoLimitGroupTests, SubsystemSuffix) {
	std::stringstream input("123:blkioo:/test");
	EXPECT_THROW(getIoLimitGroupId(input, "blkio"), GetIoLimitGroupIdException);
}

TEST(IoLimitGroupTests, SubsystemPrefix) {
	std::stringstream input("123:bblkio:/test");
	EXPECT_THROW(getIoLimitGroupId(input, "blkio"), GetIoLimitGroupIdException);
}

TEST(IoLimitGroupTests, Minimal) {
	std::stringstream input(":blkio:/test\n");
	EXPECT_EQ(getIoLimitGroupId(input, "blkio"), "/test");
}

TEST(IoLimitGroupTests, Commas) {
	std::stringstream input("123:cpuset,blkio,memory:/test\n");
	EXPECT_EQ(getIoLimitGroupId(input, "blkio"), "/test");
}

TEST(IoLimitGroupTests, SecondLine) {
	std::stringstream input("1:blah:/wrong\n:blkio:/test\n");
	EXPECT_EQ(getIoLimitGroupId(input, "blkio"), "/test");
}

TEST(IoLimitGroupTests, CgroupV2) {
	std::stringstream input("0::/test/path\n");
	EXPECT_EQ(getIoLimitGroupId(input, "blkio"), "/test/path");
}

TEST(IoLimitGroupTests, CgroupV2AndV1) {
	std::stringstream input("0::/v2/path\n1:blkio:/v1/path\n");
	// V1 should take precedence
	EXPECT_EQ(getIoLimitGroupId(input, "blkio"), "/v1/path");
}

TEST(IoLimitGroupTests, CgroupV2AndOtherV1) {
	std::stringstream input("0::/v2/path\n1:cpu:/v1/cpu/path\n");
	// V1 doesn't match requested, should fallback to v2
	EXPECT_EQ(getIoLimitGroupId(input, "blkio"), "/v2/path");
}

TEST(IoLimitGroupTests, CgroupHybridReversed) {
	std::stringstream input("1:blkio:/v1/path\n0::/v2/path\n");
	// V1 should still take precedence if found first
	EXPECT_EQ(getIoLimitGroupId(input, "blkio"), "/v1/path");
}

TEST(IoLimitGroupTests, MalformedLine) {
	std::stringstream input("invalid_line\n0::/valid/v2\n");
	EXPECT_EQ(getIoLimitGroupId(input, "blkio"), "/valid/v2");
}
