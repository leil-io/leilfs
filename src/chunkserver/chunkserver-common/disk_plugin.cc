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
	if (!initializeLogger()) {
		return false;
	}

	// Also needed after upgrading to c++23
	initializeEmptyBlockCrcForDisks();

	return true;
}

bool DiskPlugin::initializeLogger() {
	logger_ = spdlog::get("syslog");

	if (!logger_) {
		// spdlog's registry can be per-shared-library when used as a
		// header-only lib, so `get` may return null even though another
		// module already registered "syslog". Catch the duplicate-name
		// exception and fall back to `get` in that case.
		try {
			logger_ = spdlog::syslog_logger_mt("syslog");
		} catch (const spdlog::spdlog_ex &e) {
			logger_ = spdlog::get("syslog");

			if (!logger_) {
				safs::log_warn(
				    "Disk plugin: failed to create or obtain 'syslog' spdlog logger after exception: {}",
				    e.what());
				return false;
			}
		}
	}

	return true;
}

std::string DiskPlugin::toString() {
	return name() + " v" + SAUNAFS_PACKAGE_VERSION;
}
