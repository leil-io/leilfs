/*


 Copyright 2023 Leil Storage OÜ

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

#include <map>
#include <memory>
#include <string>

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

inline MountInfo mountInfo;
inline std::string mountInfoStr;

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

inline void setMountInfo(
    const std::string &startedDateUtc,
#ifdef _WIN32
    const std::string &sid,
#else
    const int &uid, const int &gid,
#endif
    const std::string &username, int pid, const std::string &version, const std::string &commitId,
    const std::string &arguments,
    std::unique_ptr<std::map<std::string, std::string>> mountOptions = nullptr) {
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
