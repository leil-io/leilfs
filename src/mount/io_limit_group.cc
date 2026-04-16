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

#include "common/platform.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "mount/io_limit_group.h"

static bool searchSubsystems(const std::string& subsystems, const std::string& subsystem) {
	std::stringstream ss(subsystems);
	std::string sub;
	while (std::getline(ss, sub, ',')) {
		if (sub == subsystem) {
			return true;
		}
	}
	return false;
}

IoLimitGroupId getIoLimitGroupId(std::istream& input, const std::string& subsystem) {
	std::string v2Path;
	bool v2Found = false;

	try {
		for (std::string line; std::getline(input, line);) {
			try {
				std::stringstream ss(line);

				std::string hierarchyIdStr;
				if (!std::getline(ss, hierarchyIdStr, ':')) {
					continue;
				}

				std::string subsystems;
				if (!std::getline(ss, subsystems, ':')) {
					continue;
				}

				std::string path;
				if (!std::getline(ss, path)) {
					continue;
				}

				if (hierarchyIdStr == "0" && subsystems.empty()) {
					v2Path = path;
					v2Found = true;
				}

				if (searchSubsystems(subsystems, subsystem)) {
					return path;
				}
			} catch (std::ios_base::failure&) {
				throw GetIoLimitGroupIdException("Parse error");
			}
		}
	} catch (std::exception&) {
		// Under clang std::getline can throw other exception
		// than std::ios_base::failure when encountering eof.
		if (!input.eof()) {
			throw;
		}
	}

	if (v2Found) {
		return v2Path;
	}

	throw GetIoLimitGroupIdException("Can't find subsystem '" + subsystem + "'");
}

IoLimitGroupId getIoLimitGroupId(const pid_t pid, const std::string& subsystem) {
	char filename[32];
	sprintf(filename, "/proc/%u/cgroup", (unsigned)pid);
	try {
		std::ifstream ifs;
		ifs.exceptions(std::ifstream::failbit | std::ifstream::badbit);
		ifs.open(filename);
		return getIoLimitGroupId(ifs, subsystem);
	} catch (std::ios_base::failure& ex) {
		throw GetIoLimitGroupIdException(
				"Error reading '" + std::string(filename) + ": " + ex.what());
	}
}

IoLimitGroupId getIoLimitGroupIdNoExcept(const pid_t pid, const std::string& subsystem) {
	try {
		return getIoLimitGroupId(pid, subsystem);
	} catch (GetIoLimitGroupIdException&) {
		return kUnclassified;
	}
}
