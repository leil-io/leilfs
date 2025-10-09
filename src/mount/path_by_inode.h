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

#include "common/platform.h"

#include "common/type_defs.h"

#include <mutex>
#include <string>

struct PidPathEntry {
	pid_t pid;
	std::string path;
	bool operator<(const PidPathEntry &other) const { return pid < other.pid; }
};

struct InodePathInfo {
	// This will store for each PID for a path by inode request, how many times that path was
	// requested This helps to manage multiple requests triggered from same PID
	std::map<PidPathEntry, uint64_t> contextPidToPath;
	std::mutex mtx;
};

inline InodePathInfo gInodePathInfo;
