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

#include "common/platform.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include "mount/exports.h"
#include "mount/fuse/sfs_fuselib/metadata.h"

namespace {

void checkGenericStat(struct stat fileStat) {
	EXPECT_EQ(fileStat.st_size, 0U);
	EXPECT_EQ(fileStat.st_blocks, 0U);
	EXPECT_TRUE(fileStat.st_uid == 0U);
	EXPECT_TRUE(fileStat.st_gid == 0U);
}

}  // namespace

TEST(MetadataMount, RootDirFiles) {
	auto rootEntries = rootDirEntries();

	EXPECT_EQ(rootEntries.at(0).inode, 0x01U);
	EXPECT_EQ(rootEntries.at(1).inode, 0x01U);
	EXPECT_EQ(rootEntries.at(2).inode, 0xFFFFFFF5U);
	EXPECT_EQ(rootEntries.at(3).inode, 0xFFFFFFF7U);

	EXPECT_EQ(rootEntries.at(2).name, "trash");
	EXPECT_EQ(rootEntries.at(3).name, "reserved");

	for (const auto &entry: rootEntries) {
		EXPECT_EQ(entry.type, 'd');
	}
}

TEST(MetadataMount, FileStatRootDir) {
	struct stat fileStat{};
	nonRootAllowedToUseMeta() = false;

	sfsMetaStat(SPECIAL_INODE_ROOT, &fileStat);

	EXPECT_EQ(fileStat.st_ino, SPECIAL_INODE_ROOT);
	EXPECT_EQ(fileStat.st_nlink, 4U);
	// S_IFDIR (040000) | META_ROOT_MODE (0555)
	EXPECT_EQ(fileStat.st_mode, 040555U);

	checkGenericStat(fileStat);
}

TEST(MetadataMount, FileStatTrashDir) {
	struct stat fileStat{};
	nonRootAllowedToUseMeta() = false;

	sfsMetaStat(SPECIAL_INODE_META_TRASH, &fileStat);

	EXPECT_EQ(fileStat.st_ino, SPECIAL_INODE_META_TRASH);
	EXPECT_EQ(fileStat.st_nlink, 3U);
	// S_IFDIR (040000) | 0700
	EXPECT_EQ(fileStat.st_mode, 040700U);

	nonRootAllowedToUseMeta() = true;
	sfsMetaStat(SPECIAL_INODE_META_TRASH, &fileStat);
	// S_IFDIR (040000) | 0777
	EXPECT_EQ(fileStat.st_mode, 040777U);

	checkGenericStat(fileStat);
}

TEST(MetadataMount, FileStatUndelDir) {
	struct stat fileStat{};
	nonRootAllowedToUseMeta() = false;

	sfsMetaStat(SPECIAL_INODE_META_UNDEL, &fileStat);

	EXPECT_EQ(fileStat.st_ino, SPECIAL_INODE_META_UNDEL);
	EXPECT_EQ(fileStat.st_nlink, 2U);
	// S_IFDIR (040000) | 0200
	EXPECT_EQ(fileStat.st_mode, 040200U);

	nonRootAllowedToUseMeta() = true;
	sfsMetaStat(SPECIAL_INODE_META_TRASH, &fileStat);
	// S_IFDIR (040000) | 0777
	EXPECT_EQ(fileStat.st_mode, 040777U);

	checkGenericStat(fileStat);
}

TEST(MetadataMount, FileStatReservedDir) {
	struct stat fileStat{};
	nonRootAllowedToUseMeta() = false;

	sfsMetaStat(SPECIAL_INODE_META_RESERVED, &fileStat);

	EXPECT_EQ(fileStat.st_ino, SPECIAL_INODE_META_RESERVED);
	EXPECT_EQ(fileStat.st_nlink, 2U);
	// S_IFDIR (040000) | 0500
	EXPECT_EQ(fileStat.st_mode, 040500U);

	nonRootAllowedToUseMeta() = true;
	sfsMetaStat(SPECIAL_INODE_META_RESERVED, &fileStat);
	// S_IFDIR (040000) | 0555
	EXPECT_EQ(fileStat.st_mode, 040555U);

	checkGenericStat(fileStat);
}

TEST(MetadataMount, MetaNameToInode) {
	inode_t inode = 0;
	inode = metadataNameToInode("0000000000005431| test");
	EXPECT_EQ(inode, 0x5431U);
	inode = metadataNameToInode("abc test");
	EXPECT_EQ(inode, 0x0U);
	inode = metadataNameToInode("454est");
	EXPECT_EQ(inode, 0x0U);
	inode = metadataNameToInode("000454|");
	EXPECT_EQ(inode, 0x0U);
	inode = metadataNameToInode("000454");
	EXPECT_EQ(inode, 0x0U);
}

TEST(MetadataMount, ResetStat) {
	struct stat fileStat{};
	unsigned long expectInode = 5;
	fileStat.st_ino = 1234;
	fileStat.st_mode = S_IFDIR;
	fileStat.st_gid = 1000;
	resetStat(expectInode, 'f', fileStat);
	EXPECT_EQ(fileStat.st_gid, 0U);
	EXPECT_EQ(fileStat.st_mode, 0100000U);
	EXPECT_EQ(fileStat.st_ino, 5U);

	resetStat(1, 'd', fileStat);
	EXPECT_EQ(fileStat.st_mode, 0040000U);

	resetStat(1, 'l', fileStat);
	EXPECT_EQ(fileStat.st_mode, 0120000U);

	resetStat(1, 'f', fileStat);
	EXPECT_EQ(fileStat.st_mode, 0100000U);

	resetStat(1, 'q', fileStat);
	EXPECT_EQ(fileStat.st_mode, 0010000U);

	resetStat(1, 's', fileStat);
	EXPECT_EQ(fileStat.st_mode, 0140000U);

	resetStat(1, 'b', fileStat);
	EXPECT_EQ(fileStat.st_mode, 0060000U);

	resetStat(1, 'c', fileStat);
	EXPECT_EQ(fileStat.st_mode, 0020000U);

	resetStat(1, '\t', fileStat);
	EXPECT_EQ(fileStat.st_mode, 0U);
}

TEST(MetadataMount, MasterInfoStat) {
	struct stat fileStat = getMasterInfoStat();
	EXPECT_EQ(fileStat.st_ino, 0xFFFFFFFF);
	EXPECT_EQ(fileStat.st_mode, 0100444U);
	EXPECT_EQ(fileStat.st_uid, 0U);
	EXPECT_EQ(fileStat.st_gid, 0U);
	EXPECT_EQ(fileStat.st_atime, 0U);
	EXPECT_EQ(fileStat.st_mtime, 0U);
	EXPECT_EQ(fileStat.st_ctime, 0U);
	EXPECT_EQ(fileStat.st_nlink, 1U);
	EXPECT_EQ(fileStat.st_size, 14);
	EXPECT_EQ(fileStat.st_blocks, 1);
}
