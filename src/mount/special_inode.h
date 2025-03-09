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

#pragma once

#include "common/platform.h"

#include "common/special_inode_defs.h"
#include "mount/sauna_client.h"
#include "mount/sauna_client_context.h"
#include "mount/mastercomm.h"
#include "mount/masterproxy.h"
#include "mount/oplog.h"
#include "mount/tweaks.h"

namespace InodeMasterInfo {
	extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

namespace InodeStats {
	typedef struct _sinfo {
		char *buff;
		uint32_t leng;
		uint8_t reset;
		pthread_mutex_t lock;
	} sinfo;

	extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

namespace InodeOplog {
	extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

namespace InodeOphistory {
	extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

namespace InodeTweaks {
#ifdef _WIN32
	static inline bool isWStringFromWindows(const std::string &value) {
		// Detecting Windows API wide string format
		return !value.empty() && value[0] == -1;
	}

	static inline std::string convertWStringFromWindowsToString(
		const std::string &value) {
		// This conversion is necessary due to the special encoding format
		// of Windows API strings, which may include a Byte Order Mark (BOM)
		// and use UTF-16 encoding. The conversion ensures compatibility
		// with UTF-8 encoded strings, which are more universally supported.
		std::string result;
		for (int i = 2; i < (int)value.size(); i += 2) {
			result.push_back(value[i]);
		}
		return result;
	}
#endif

	extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

namespace InodeFileByInode {
	extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

namespace InodePathByInode {
	typedef struct _inodePathInfo {
		char *pathByInode;
		uint32_t inode;
		std::mutex mtx;
		std::condition_variable cv;
		bool locked = false;
	} InodePathInfo;
    extern InodePathInfo inodePathInfo;

    extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

namespace InodeMountInfo {
	typedef struct _mountInfo {
		std::string startedDateUtc;
#ifdef _WIN32
		std::string sid;
#else
		std::string uid;
		std::string gid;
#endif
		std::string username;
		int pid;
		std::string version;
		std::string commitId;
		std::string arguments;
		std::unique_ptr<std::map<std::string, std::string>> mountOptions;
	} MountInfo;
	extern MountInfo mountInfo;
	extern std::string mountInfoStr;

	inline void buildMountInfoStr() {
		mountInfoStr.clear();
		mountInfoStr = "SAUNAFS CLIENT MOUNT INFO:\n";
		mountInfoStr += "--------------------------------\n";
		mountInfoStr += "STARTED DATE: " + mountInfo.startedDateUtc + "\n";
#ifdef _WIN32
		mountInfoStr += "SID: " + mountInfo.sid + "\n";
#else
		mountInfoStr += "UID: " + mountInfo.uid + "\n";
		mountInfoStr += "GID: " + mountInfo.gid + "\n";
#endif
		mountInfoStr += "USERNAME: " + mountInfo.username + "\n";
		mountInfoStr += "PID: " + std::to_string(mountInfo.pid) + "\n";
		mountInfoStr += "VERSION: " + mountInfo.version + "\n";
		mountInfoStr += "COMMIT_ID: " + mountInfo.commitId + "\n";
		mountInfoStr += "ARGUMENTS: " + mountInfo.arguments + "\n";
		if (mountInfo.mountOptions) {
			mountInfoStr += "MOUNT OPTIONS:\n";
			for (const auto &opt : *mountInfo.mountOptions) {
				mountInfoStr += opt.first + ": " + opt.second + "\n";
			}
		}
		mountInfoStr += "--------------------------------\n";
	}

	inline void setMountInfo(const std::string &startedDateUtc,
#ifdef _WIN32
		const std::string &sid,
#else
		const int &uid, const int &gid,
#endif
		const std::string &username, int pid,
		const std::string &version, const std::string &commitId,
		const std::string &arguments, std::unique_ptr<std::map<std::string, std::string>> mountOptions = nullptr) {
		mountInfo.startedDateUtc = startedDateUtc;
#ifdef _WIN32
		mountInfo.sid = sid;
#else
		mountInfo.uid = std::to_string(uid);
		mountInfo.gid = std::to_string(gid);
#endif
		mountInfo.username = username;
		mountInfo.pid = pid;
		mountInfo.version = version;
		mountInfo.commitId = commitId;
		mountInfo.arguments = arguments;
		mountInfo.mountOptions = std::move(mountOptions);
		buildMountInfoStr();
	}

	extern const Attributes attr;
	extern const SaunaClient::Inode inode_;
}

std::vector<uint8_t> special_read(SaunaClient::Inode ino, const SaunaClient::Context &ctx,
	                          size_t size, off_t off, SaunaClient::FileInfo *fi, int debug_mode);

SaunaClient::BytesWritten special_write(SaunaClient::Inode ino, const SaunaClient::Context &ctx,
	                                 const char *buf, size_t size, off_t off, SaunaClient::FileInfo *fi);

SaunaClient::EntryParam special_lookup(SaunaClient::Inode ino, const SaunaClient::Context &ctx,
	                                SaunaClient::Inode parent, const char *name, char attrstr[256]);

SaunaClient::AttrReply special_getattr(SaunaClient::Inode ino, const SaunaClient::Context &ctx,
	                                char (&attrstr)[256]);

SaunaClient::AttrReply special_setattr(SaunaClient::Inode ino, const SaunaClient::Context &ctx, struct stat *stbuf,
	                                int to_set, char modestr[11], char attrstr[256]);

void special_open(SaunaClient::Inode ino, const SaunaClient::Context &ctx, SaunaClient::FileInfo *fi);

void special_release(SaunaClient::Inode ino, SaunaClient::FileInfo *fi);
