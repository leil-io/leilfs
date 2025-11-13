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

#pragma once

#include "common/platform.h"

#include <fcntl.h>
#include <sys/stat.h>

#include "protocol/SFSCommunication.h"

#include <array>
#include <cstdint>

inline constexpr size_t kAttributesSize = 35;
using Attributes = std::array<uint8_t, kAttributesSize>;

#ifndef _WIN32
constexpr int saunaFileTypeToPosix(unsigned char type) {
	switch (type) {
	case TYPE_BLOCKDEV:
		return S_IFBLK;
	case TYPE_CHARDEV:
		return S_IFCHR;
	case TYPE_DIRECTORY:
		return S_IFDIR;
	case TYPE_FIFO:
		return S_IFIFO;
	case TYPE_FILE:
		return S_IFREG;
	case TYPE_SOCKET:
		return S_IFSOCK;
	case TYPE_SYMLINK:
		return S_IFLNK;
	default:
		return 0;
	}
}

struct PosixFileAttributes {
	uint8_t type;
	uint16_t permissions;
	uint32_t userID;
	uint32_t groupID;
	uint32_t accessTime;
	uint32_t modificationTime;
	uint32_t creationTime;
	uint32_t hardLinkCount;
	uint64_t size;

	constexpr struct stat getStat() const {
		struct stat fileStat{};
		fileStat.st_mode = saunaFileTypeToPosix(type) | permissions;
		fileStat.st_size = static_cast<off_t>(size);
		// TODO(Urmas): Why are we returning to clients blocks of 512?
		// Instead of S_BLKSIZE, it should SFSBLOCKSIZE
		fileStat.st_blocks = (static_cast<off_t>(size) + S_BLKSIZE - 1) / S_BLKSIZE;
		fileStat.st_uid = userID;
		fileStat.st_gid = groupID;
		fileStat.st_atime = accessTime;
		fileStat.st_mtime = modificationTime;
		fileStat.st_ctime = creationTime;
		fileStat.st_nlink = hardLinkCount;
		return fileStat;
	}
};
#endif
