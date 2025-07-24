/*
   Copyright 2025      Leil Storage OÜ

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <ctime>
#include <cstring>
#include <string>
#include "common/attributes.h"
#include "common/special_inode_defs.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

enum : uint16_t {
	READDIR_BUFFSIZE = 50000,
	PATH_SIZE_LIMIT = 1024,
	META_ROOT_MODE = 0555,
};

struct MetaStat {
	std::string name;
	inode_t inode{};
	char type{};
};

static constexpr std::array<MetaStat, 4> rootDirEntries() {
		auto rootDir = MetaStat(".", SPECIAL_INODE_ROOT, TYPE_DIRECTORY);
		auto upDir = MetaStat("..", SPECIAL_INODE_ROOT, TYPE_DIRECTORY);
		auto trash = MetaStat(SPECIAL_FILE_NAME_META_TRASH, SPECIAL_INODE_META_TRASH, TYPE_DIRECTORY);
		auto reserved = MetaStat(SPECIAL_FILE_NAME_META_RESERVED, SPECIAL_INODE_META_RESERVED, TYPE_DIRECTORY);
		std::array<MetaStat, 4> entries = {rootDir, upDir, trash, reserved};
		return entries;
}

void sfsMetaStat(inode_t inode, struct stat *stbuf);

uint32_t metadataNameToInode(const std::string &name);

/**
 * Memsets the stat struct to 0, and then sets the inode and SaunaFS file type.
 *
 * @param inode New inode type to set
 * @param type SaunaFS type to convert to system type
 * @param fileStat Reference to the stat structure that will be reset.
 */
void resetStat(inode_t inode, unsigned char type, struct stat &fileStat);

/**
 * Returns the stat structure with attributes for the master info file
 *
 * @returns .masterinfo stat
 */
constexpr struct stat getMasterInfoStat() {
	struct stat fileStat{};
	constexpr PosixFileAttributes attrs = {
		.type = 'f',
		.permissions = 0444,
		.userID = 0,
		.groupID = 0,
		.accessTime = 0,
		.modificationTime = 0,
		.creationTime = 0,
		.hardLinkCount = 1,
		.size = 14,
	};
	fileStat = attrs.getStat();
	fileStat.st_ino = SPECIAL_INODE_MASTERINFO;

	return fileStat;
}
