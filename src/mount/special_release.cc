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
#include "mount/stats.h"

using namespace SaunaClient;

namespace InodeMasterInfo {
static void release(FileInfo */*fi*/) {
	oplog_printf("release (%" PRIiNode ") (internal node: MASTERINFO): OK", inode_);
}
} // InodeMasterInfo

namespace InodeStats {
static void release(FileInfo *fi) {
	sinfo *statsinfo = reinterpret_cast<sinfo*>(fi->fh);
	if (statsinfo!=NULL) {
		std::unique_lock lock(statsinfo->lock);         // make helgrind happy
		if (statsinfo->buff!=NULL) {
			free(statsinfo->buff);
		}
		if (statsinfo->reset) {
			stats_reset_all();
		}
		lock.unlock(); // This unlock is needed, since we want to destroy the mutex
		delete statsinfo;
	}
	oplog_printf("release (%" PRIiNode ") (internal node: STATS): OK", inode_);
}
} // InodeStats

namespace InodeOplog {
static void release(FileInfo *fi) {
	oplog_releasehandle(fi->fh);
	oplog_printf("release (%" PRIiNode ") (internal node: OPLOG): OK", inode_);
}
} // InodeOplog

namespace InodeOphistory {
static void release(FileInfo *fi) {
	oplog_releasehandle(fi->fh);
	oplog_printf("release (%" PRIiNode ") (internal node: OPHISTORY): OK", inode_);
}
} // InodeOphistory

namespace InodeTweaks {
static void release(FileInfo *fi) {
	MagicFile *file = reinterpret_cast<MagicFile*>(fi->fh);
	if (file->wasWritten) {
#ifdef _WIN32
		if (isWStringFromWindows(file->value)) {
			file->value = convertWStringFromWindowsToString(file->value);
		}
#endif

		auto separatorPos = file->value.find('=');
		if (separatorPos == file->value.npos) {
			safs_pretty_syslog(LOG_INFO, "TWEAKS_FILE: Wrong value '%s'",
			                   file->value.c_str());
		} else {
			std::string name = file->value.substr(0, separatorPos);
			std::string value = file->value.substr(separatorPos + 1);
			if (!value.empty() && value.back() == '\n') {
				value.resize(value.size() - 1);
			}
			gTweaks.setValue(name, value);
			safs_pretty_syslog(LOG_INFO, "TWEAKS_FILE: Setting '%s' to '%s'",
			                   name.c_str(), value.c_str());
		}
	}
	delete file;
	oplog_printf("release (%" PRIiNode ") (internal node: TWEAKS_FILE): OK", inode_);
}
} // InodeTweaks

namespace InodePathByInode {
static void release(FileInfo *fi) {
	std::unique_lock<std::mutex> lock(gInodePathInfo.mtx);
	auto entry = reinterpret_cast<PidPathEntry *>(fi->fh);
	if (entry) {
		auto it = gInodePathInfo.contextPidToPath.find(*entry);
		if (it != gInodePathInfo.contextPidToPath.end() && --(it->second) <= 0) {
			gInodePathInfo.contextPidToPath.erase(it);
		}
	}
	fi->fh = 0;
	oplog_printf("release (%" PRIiNode ") (internal node: PATH_BY_INODE_FILE): OK", inode_);
}
} // InodePathByInode

namespace InodeMountInfo {
static void release(FileInfo *fi) {
	std::lock_guard lock(gMountInfoMtx);
	char *buff = reinterpret_cast<char*>(fi->fh);
	if (buff) {
		free(buff);
	}
	fi->fh = 0;
	oplog_printf("release (%" PRIiNode ") (internal node: MOUNT_INFO): OK", inode_);
}
}  // InodeMountInfo

typedef void (*ReleaseFunc)(FileInfo *);
static const std::array<ReleaseFunc, 16> funcs = {{
	 &InodeStats::release,          //0x0U
	 &InodeOplog::release,          //0x1U
	 &InodeOphistory::release,      //0x2U
	 &InodeTweaks::release,         //0x3U
	 nullptr,                       //0x4U
	 nullptr,                       //0x5U
	 nullptr,                       //0x6U
	 nullptr,                       //0x7U
	 &InodePathByInode::release,    //0x8U
	 &InodeMountInfo::release,      //0x9U
	 nullptr,                       //0xAU
	 nullptr,                       //0xBU
	 nullptr,                       //0xCU
	 nullptr,                       //0xDU
	 nullptr,                       //0xEU
	 &InodeMasterInfo::release      //0xFU
}};

void special_release(inode_t ino, FileInfo *fi) {
	auto func = funcs[ino - SPECIAL_INODE_BASE];
	if (!func) {
		safs_pretty_syslog(LOG_WARNING,
			"Trying to call unimplemented 'release' function for special inode");
		throw RequestException(SAUNAFS_ERROR_EINVAL);
	}
	return func(fi);
}
