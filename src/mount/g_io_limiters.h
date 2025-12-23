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

#pragma once

#include "common/platform.h"

#include "common/args_stat_encoding.h"
#include "common/io_limiting.h"
#include "mount/global_io_limiter.h"

// Mount iolimits configuration file path
inline std::string gIOLimitsConfigFilePath;
inline std::mutex gIOLimitsConfigFilePathMutex;
inline std::atomic<bool> gIOLimitsInitialized{false};

ioLimiting::MountLimiter& gMountLimiter();
ioLimiting::LimiterProxy& gLocalIoLimiter();
ioLimiting::LimiterProxy& gGlobalIoLimiter();

inline void fsLoadMountIoLimits() {
	std::unique_lock ioLimitsLock(gIOLimitsConfigFilePathMutex);
	IoLimitsConfigLoader loader;

	if (!gIOLimitsConfigFilePath.empty()) {
		std::string fileContent;
		if (!read_file_to_string(gIOLimitsConfigFilePath, fileContent)) {
			const char *strError = std::strerror(errno);
			safs::log_warn(
			    "iolimits: cannot open I/O limits configuration file '{}': {}; using master-provided limits if available, otherwise no client-side limiting.",
			    gIOLimitsConfigFilePath, strError);
		} else {
			try {
				std::istringstream iss(fileContent);
				loader.load(std::move(iss));
			} catch (const Exception &ex) {
				safs::log_warn(
				    "iolimits: failed to parse I/O limits configuration file '{}': {}; using master-provided limits if available, otherwise no client-side limiting.",
				    gIOLimitsConfigFilePath, ex.what());
			}
		}
	}

	gMountLimiter().loadConfiguration(loader);
}
