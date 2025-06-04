/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2016 Skytechnology sp. z o.o.
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

#include "mount/client_common.h"
#include "mount/special_inode.h"

using namespace SaunaClient;

static void printSetattrOplog(const Context &ctx, inode_t ino, struct stat *stbuf, int to_set,
	                    const char modestr[11], const char attrstr[256], const char *node_name) {

	oplog_printf(ctx, "setattr (%" PRIiNode ",0x%X,[%s:0%04o,%ld,%ld,%lu,%lu,%" PRIu64 "]) (internal node: %s): OK (3600,%s)",
	            ino,
	            to_set,
	            (modestr + 1),
	            (unsigned int)(stbuf->st_mode & 07777),
	            (long int)stbuf->st_uid,
	            (long int)stbuf->st_gid,
	            (unsigned long int)(stbuf->st_atime),
	            (unsigned long int)(stbuf->st_mtime),
	            (uint64_t)(stbuf->st_size),
	            node_name,
	            attrstr);
}

namespace InodeMasterInfo {
static AttrReply setattr(const Context &ctx, struct stat *stbuf, int to_set,
	                 char modestr[11], char /*attrstr*/[256]) {
	oplog_printf(ctx, "setattr (%" PRIiNode ",0x%X,[%s:0%04o,%ld,%ld,%lu,%lu,%" PRIu64 "]): %s",
	            inode_,
	            to_set,
	            (modestr + 1),
	            (unsigned int)(stbuf->st_mode & 07777),
	            (long int)stbuf->st_uid,
	            (long int)stbuf->st_gid,
	            (unsigned long int)(stbuf->st_atime),
	            (unsigned long int)(stbuf->st_mtime),
	            (uint64_t)(stbuf->st_size),
	            saunafs_error_string(SAUNAFS_ERROR_EPERM));
	throw RequestException(SAUNAFS_ERROR_EPERM);
}
} // InodeMasterInfo

namespace InodeStats {
static AttrReply setattr(const Context &ctx, struct stat *stbuf, int to_set,
	                 char modestr[11], char attrstr[256]) {
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	makeattrstr(attrstr, 256, &o_stbuf);
	printSetattrOplog(ctx, inode_, stbuf, to_set, modestr, attrstr, "STATS");
	return AttrReply{o_stbuf, 3600.0};
}
} // InodeStats

namespace InodeOplog {
static AttrReply setattr(const Context &ctx, struct stat *stbuf, int to_set,
	                 char modestr[11], char attrstr[256]) {
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	makeattrstr(attrstr, 256, &o_stbuf);
	printSetattrOplog(ctx, inode_, stbuf, to_set, modestr, attrstr, "OPLOG");
	return AttrReply{o_stbuf, 3600.0};
}
} // InodeOplog

namespace InodeOphistory {
static AttrReply setattr(const Context &ctx, struct stat *stbuf, int to_set,
	                 char modestr[11], char attrstr[256]) {
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	makeattrstr(attrstr, 256, &o_stbuf);
	printSetattrOplog(ctx, inode_, stbuf, to_set, modestr, attrstr, "OPHISTORY");
	return AttrReply{o_stbuf, 3600.0};
}
} // InodeOphistory

namespace InodeTweaks {
static AttrReply setattr(const Context &ctx, struct stat *stbuf, int to_set,
	                 char modestr[11], char attrstr[256]) {
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	makeattrstr(attrstr, 256, &o_stbuf);
	printSetattrOplog(ctx, inode_, stbuf, to_set, modestr, attrstr, "TWEAKS_FILE");
	return AttrReply{o_stbuf, 3600.0};
}
} // InodeTweaks

namespace InodeFileByInode {
static AttrReply setattr(const Context &ctx, struct stat *stbuf, int to_set,
	                 char modestr[11], char attrstr[256]) {
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	makeattrstr(attrstr, 256, &o_stbuf);
	printSetattrOplog(ctx, inode_, stbuf, to_set, modestr, attrstr, "FILE_BY_INODE_FILE");
	return AttrReply{o_stbuf, 3600.0};
}
} // InodeFileByInode

namespace InodePathByInode {
static AttrReply setattr(const Context &ctx, struct stat *stbuf, int to_set,
	                 char modestr[11], char attrstr[256]) {
	std::unique_lock<std::mutex> lock(inodePathInfo.mtx);
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	makeattrstr(attrstr, 256, &o_stbuf);
	printSetattrOplog(ctx, inode_, stbuf, to_set, modestr, attrstr, "PATH_BY_INODE_FILE");
	return AttrReply{o_stbuf, 3600.0};
}
} // InodePathByInode

namespace InodeMountInfo {
static AttrReply setattr(const Context &ctx, struct stat *stbuf, int to_set, char modestr[11],
                         char attrstr[256]) {
	std::lock_guard lock(gMountInfoMtx);
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	makeattrstr(attrstr, 256, &o_stbuf);
	printSetattrOplog(ctx, inode_, stbuf, to_set, modestr, attrstr, "MOUNT_INFO");
	return AttrReply{o_stbuf, 3600.0};
}
}  // InodeMountInfo

static const std::array<std::function<AttrReply
	(const Context&, struct stat*, int, char[11], char[256])>, 16> funcs = {{
	 &InodeStats::setattr,          //0x0U
	 &InodeOplog::setattr,          //0x1U
	 &InodeOphistory::setattr,      //0x2U
	 &InodeTweaks::setattr,         //0x3U
	 &InodeFileByInode::setattr,    //0x4U
	 nullptr,                       //0x5U
	 nullptr,                       //0x6U
	 nullptr,                       //0x7U
	 &InodePathByInode::setattr,    //0x8U
	 &InodeMountInfo::setattr,      //0x9U
	 nullptr,                       //0xAU
	 nullptr,                       //0xBU
	 nullptr,                       //0xCU
	 nullptr,                       //0xDU
	 nullptr,                       //0xEU
	 &InodeMasterInfo::setattr      //0xFU
}};

AttrReply special_setattr(inode_t ino, const Context &ctx, struct stat *stbuf, int to_set,
	                  char modestr[11], char attrstr[256]) {
	auto func = funcs[ino - SPECIAL_INODE_BASE];
	if (!func) {
		safs_pretty_syslog(LOG_WARNING,
			"Trying to call unimplemented 'setattr' function for special inode");
		throw RequestException(SAUNAFS_ERROR_EINVAL);
	}
	return func(ctx, stbuf, to_set, modestr, attrstr);
}
