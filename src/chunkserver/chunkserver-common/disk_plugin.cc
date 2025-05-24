/*
   Copyright 2023 Leil Storage

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

#include "chunkserver-common/disk_plugin.h"

#include "chunkserver-common/disk_with_fd.h"

DiskPlugin::DiskPlugin() {}

DiskPlugin::~DiskPlugin() {}

bool DiskPlugin::initialize() {
	// Needed after upgrading to c++23
	// https://github.com/gabime/spdlog/wiki/How-to-use-spdlog-in-DLLs
	initializeLogger();

	// Also needed after upgrading to c++23
	initializeEmptyBlockCrcForDisks();

	return true;
}

void DiskPlugin::initializeLogger() {
		logger_ = spdlog::get("syslog");

		if (!logger_) {
			logger_ = spdlog::syslog_logger_mt("syslog");
		}
	}

std::string DiskPlugin::toString() {
	return name() + " v" + SAUNAFS_PACKAGE_VERSION;
}
