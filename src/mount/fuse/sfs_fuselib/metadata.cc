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

#include "common/platform.h"

#include <sys/stat.h>
#include <cstdint>
#include <cstdlib>
#include <string>
#include "mount/fuse/sfs_fuselib/metadata.h"

#include "common/attributes.h"
#include "common/special_inode_defs.h"
#include "mount/exports.h"

void sfsMetaStat(inode_t inode, struct stat *stbuf) {
	time_t now = time(nullptr);
	constexpr int allowAll = 0777;
	constexpr int allowReadExecAll = 0555;
	constexpr int allowReadExecRoot = 0500;
	constexpr int allowRootAll = 0700;
	constexpr int allowRootWriteOnly = 0200;

	stbuf->st_ino = inode;
	stbuf->st_size = 0;
	stbuf->st_blocks = 0;
	stbuf->st_uid = 0;
	stbuf->st_gid = 0;
	stbuf->st_atime = now;
	stbuf->st_mtime = now;
	stbuf->st_ctime = now;

	switch (inode) {
	case SPECIAL_INODE_ROOT:
		stbuf->st_nlink = 4;
		stbuf->st_mode = S_IFDIR | META_ROOT_MODE;
		break;
	case SPECIAL_INODE_META_TRASH:
		stbuf->st_nlink = 3;
		stbuf->st_mode = S_IFDIR | (nonRootAllowedToUseMeta() ? allowAll : allowRootAll);
		break;
	case SPECIAL_INODE_META_UNDEL:
		stbuf->st_nlink = 2;
		stbuf->st_mode = S_IFDIR | (nonRootAllowedToUseMeta() ? allowAll : allowRootWriteOnly);
		break;
	case SPECIAL_INODE_META_RESERVED:
		stbuf->st_nlink = 2;
		stbuf->st_mode = S_IFDIR | (nonRootAllowedToUseMeta() ? allowReadExecAll : allowReadExecRoot);
		break;
	default:
		break;
	}
}

inode_t metadataNameToInode(const std::string &name) {
	inode_t inode = 0;
	size_t end = 0;
	const int base16 = 16;

	try {
		inode = std::stoul(name, &end, base16);
		if (name.at(end) == '|' && name.at(end + 1) != 0) {
			return inode;
		}
	} catch (std::exception &e) {
		safs::log_err("Could not convert metadata name to inode: {}", e.what());
	}

	return 0;
}

void resetStat(inode_t inode, unsigned char type, struct stat &fileStat) {
	memset(&fileStat, 0 ,sizeof(struct stat));
	fileStat.st_ino = inode;
	fileStat.st_mode = saunaFileTypeToPosix(type);
}
