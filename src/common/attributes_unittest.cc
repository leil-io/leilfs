/*
   Copyright 2025      Leil Storage OÜ

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

// NOLINTBEGIN

#include <gtest/gtest.h>

#include "common/attributes.h"

TEST(Attributes, TypeToPosix) {

	EXPECT_EQ(saunaFileTypeToPosix('q'), 0010000);
	EXPECT_EQ(saunaFileTypeToPosix('c'), 0020000);
	EXPECT_EQ(saunaFileTypeToPosix('d'), 0040000);
	EXPECT_EQ(saunaFileTypeToPosix('b'), 0060000);
	EXPECT_EQ(saunaFileTypeToPosix('f'), 0100000);
	EXPECT_EQ(saunaFileTypeToPosix('l'), 0120000);
	EXPECT_EQ(saunaFileTypeToPosix('s'), 0140000);
	EXPECT_EQ(saunaFileTypeToPosix('\t'), 0);

}

TEST(Attributes, PosixFileAttrsToStat) {
	PosixFileAttributes attrs = {
		.type = 'f',
		.permissions = 0456,
		.userID = 567,
		.groupID = 234,
		.accessTime = 10,
		.modificationTime = 412,
		.creationTime = 794,
		.hardLinkCount = 50,
		.size = 1024,
	};
	struct stat converted = attrs.getStat();

	EXPECT_EQ(converted.st_mode, 0100456U);
	EXPECT_EQ(converted.st_size, 1024);
	EXPECT_EQ(converted.st_uid, attrs.userID);
	EXPECT_EQ(converted.st_gid, attrs.groupID);
	EXPECT_EQ(converted.st_atime, attrs.accessTime);
	EXPECT_EQ(converted.st_mtime, attrs.modificationTime);
	EXPECT_EQ(converted.st_ctime, attrs.creationTime);
	EXPECT_EQ(converted.st_nlink, attrs.hardLinkCount);
	EXPECT_EQ(converted.st_blocks, 2U);

	attrs.size = 1025;
	converted = attrs.getStat();
	EXPECT_EQ(converted.st_blocks, 3U);
	attrs.size = 0;
	converted = attrs.getStat();
	EXPECT_EQ(converted.st_blocks, 0U);
	attrs.size = 1;
	converted = attrs.getStat();
	EXPECT_EQ(converted.st_blocks, 1U);
	attrs.size = 512;
	converted = attrs.getStat();
	EXPECT_EQ(converted.st_blocks, 1U);

}

// NOLINTEND
