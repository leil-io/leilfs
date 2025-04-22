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

#ifndef _WIN32
#include <pwd.h>
#else
#include <windows.h>
#include <iostream>
#include <lmcons.h>
#include <sddl.h> 
#endif
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "mount/tweaks.h"

class MountInfo {
public:
	MountInfo();

	std::string getStartedDateUtc() const;
#ifdef _WIN32
	std::string getSid() const;
#else
	std::string getUid() const;
	std::string getGid() const;
#endif
	std::string getUsername() const;
	int getPid() const;
	std::string getVersion() const;
	std::string getCommitId() const;
	std::string getArguments() const;
	std::map<std::string, std::string> getMountOptions() const;
	std::string getMountInfoStr();

	void setStartedDateUtc(const std::string &date);
#ifdef _WIN32
	void setSid(const std::string &sidValue);
#else
	void setUid(const std::string &uidValue);
	void setGid(const std::string &gidValue);
#endif
	void setUsername(const std::string &user);
	void setPid(int processId);
	void setVersion(const std::string &ver);
	void setCommitId(const std::string &commit);
	void setArguments(const std::string &args);
	void setMountOptions(const std::map<std::string, std::string> &options);
	void buildMountInfoStr();

private:
	std::string startedDateUtc_;
#ifdef _WIN32
	std::string sid_;
#else
	std::string uid_;
	std::string gid_;
#endif
	std::string username_;
	int pid_;
	std::string version_;
	std::string commitId_;
	std::string arguments_;
	std::unique_ptr<std::map<std::string, std::string>> mountOptions_;
	std::string mountInfoStr_;
};

// Global variables and utility functions
inline MountInfo gMountInfo;
inline std::mutex gMountInfoMtx;

void mount_info_init(
#ifdef _WIN32
	const std::string &sid,
#else
	const int &uid, const int &gid,
#endif
	const std::string &username, int pid, const std::string &version,
	const std::string &commitId);

#ifdef _WIN32
std::string get_username();
std::string get_current_user_sid();
#else
std::string get_username_by_uid(uid_t uid);
#endif
void set_all_mountpoint_arguments(int argc, char **argv);
void set_current_local_time();
