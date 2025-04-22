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

namespace InodeMasterInfo {
static AttrReply getattr(const Context &ctx, char (&attrstr)[256]) {
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	stats_inc(OP_GETATTR);
	makeattrstr(attrstr, 256, &o_stbuf);
	oplog_printf(ctx, "getattr (%lu) (internal node: MASTERINFO): OK (3600,%s)",
	            (unsigned long int)inode_,
	            attrstr);
	return AttrReply{o_stbuf, 3600.0};
}
} // InodeMasterInfo

namespace InodeStats {
static AttrReply getattr(const Context &ctx, char (&attrstr)[256]) {
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	stats_inc(OP_GETATTR);
	makeattrstr(attrstr, 256, &o_stbuf);
	oplog_printf(ctx, "getattr (%lu) (internal node: STATS): OK (3600,%s)",
	            (unsigned long int)inode_,
	            attrstr);
	return AttrReply{o_stbuf, 3600.0};
}
} // InodeStats

namespace InodeOplog {
static AttrReply getattr(const Context &ctx, char (&attrstr)[256]) {
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	stats_inc(OP_GETATTR);
	makeattrstr(attrstr, 256, &o_stbuf);
	oplog_printf(ctx, "getattr (%lu) (internal node: OPLOG): OK (3600,%s)",
	            (unsigned long int)inode_,
	            attrstr);
	return AttrReply{o_stbuf, 3600.0};
}
} // InodeOplog

namespace InodeOphistory {
static AttrReply getattr(const Context &ctx, char (&attrstr)[256]) {
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	stats_inc(OP_GETATTR);
	makeattrstr(attrstr, 256, &o_stbuf);
	oplog_printf(ctx, "getattr (%lu) (internal node: OPHISTORY): OK (3600,%s)",
	            (unsigned long int)inode_,
	            attrstr);
	return AttrReply{o_stbuf, 3600.0};
}
} // InodeOphistory

namespace InodeTweaks {
static AttrReply getattr(const Context &ctx, char (&attrstr)[256]) {
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	stats_inc(OP_GETATTR);
	makeattrstr(attrstr, 256, &o_stbuf);
	oplog_printf(ctx, "getattr (%lu) (internal node: TWEAKS_FILE): OK (3600,%s)",
	            (unsigned long int)inode_,
	            attrstr);
	return AttrReply{o_stbuf, 3600.0};
}
} // InodeTweaks

namespace InodeFileByInode {
static AttrReply getattr(const Context &ctx, char (&attrstr)[256]) {
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	stats_inc(OP_GETATTR);
	makeattrstr(attrstr, 256, &o_stbuf);
	oplog_printf(ctx, "getattr (%lu) (internal node: FILE_BY_INODE_FILE): OK (3600,%s)",
	            (unsigned long int)inode_,
	            attrstr);
	return AttrReply{o_stbuf, 3600.0};
}
} // InodeFileByInode

namespace InodePathByInode {
static AttrReply getattr(const Context &ctx, char (&attrstr)[256]) {
	std::unique_lock<std::mutex> lock(inodePathInfo.mtx);
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	stats_inc(OP_GETATTR);
	makeattrstr(attrstr, 256, &o_stbuf);
	oplog_printf(ctx, "getattr (%lu) (internal node: PATH_BY_INODE_FILE): OK (3600,%s)",
	            (unsigned long int)inode_,
	            attrstr);
	return AttrReply{o_stbuf, 3600.0};
}
} // InodePathByInode

namespace InodeMountInfo {
static AttrReply getattr(const Context &ctx, char (&attrstr)[256]) {
	std::lock_guard lock(gMountInfoMtx);
	struct stat o_stbuf;
	memset(&o_stbuf, 0, sizeof(struct stat));
	attr_to_stat(inode_, attr, &o_stbuf);
	stats_inc(OP_GETATTR);
	makeattrstr(attrstr, 256, &o_stbuf);
	oplog_printf(ctx, "getattr (%lu) (internal node: MOUNT_INFO): OK (3600,%s)",
		        (unsigned long int)inode_,
		        attrstr);
	return AttrReply{o_stbuf, 3600.0};
}
} // InodeMountInfo

typedef AttrReply (*GetAttrFunc)(const Context&, char (&)[256]);
static const std::array<GetAttrFunc, 16> funcs = {{
	 &InodeStats::getattr,          //0x0U
	 &InodeOplog::getattr,          //0x1U
	 &InodeOphistory::getattr,      //0x2U
	 &InodeTweaks::getattr,         //0x3U
	 &InodeFileByInode::getattr,    //0x4U
	 nullptr,                       //0x5U
	 nullptr,                       //0x6U
	 nullptr,                       //0x7U
	 &InodePathByInode::getattr,    //0x8U
	 &InodeMountInfo::getattr,      //0x9U
	 nullptr,                       //0xAU
	 nullptr,                       //0xBU
	 nullptr,                       //0xCU
	 nullptr,                       //0xDU
	 nullptr,                       //0xEU
	 &InodeMasterInfo::getattr      //0xFU
}};

AttrReply special_getattr(Inode ino, const Context &ctx, char (&attrstr)[256]) {
	auto func = funcs[ino - SPECIAL_INODE_BASE];
	if (!func) {
		safs_pretty_syslog(LOG_WARNING,
			"Trying to call unimplemented 'getattr' function for special inode");
		throw RequestException(SAUNAFS_ERROR_EINVAL);
	}
	return func(ctx, attrstr);
}
