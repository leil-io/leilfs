/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include "common/saunafs_version.h"
#include "master/filesystem_checksum.h"
#include "master/filesystem_checksum_background_updater.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operations_interface.h"

#ifndef METARESTORE

/*! \brief Periodically adds CHECKSUM changelog entry.
 *  Entry is added during destruction, so that it will be generated after end of function
 *  where ChecksumUpdater is created.
 */
class ChecksumUpdater {
public:
	ChecksumUpdater(uint32_t ts) : ts_(ts) {
	}

	~ChecksumUpdater() {
		if (gMetadata->metadataVersion > lastEntry_ + period_) {
			writeToChangelog(ts_);
		}
	}

	static void setPeriod(uint32_t period) {
		period_ = period;
	}

protected:
	static void writeToChangelog(uint32_t ts) {
		lastEntry_ = gMetadata->metadataVersion;
		if (metadataserver::isMaster() && !gChecksumBackgroundUpdater.inProgress()) {
			std::string versionString = saunafsVersionToString(SAUNAFS_VERSHEX);
			uint64_t checksum = gFilesystemOperations->fs_checksum(ChecksumMode::kGetCurrent);
			gFilesystemOperations->fs_changelog(ts, "CHECKSUM(%s):%" PRIu64, versionString.c_str(),
			                                    checksum);
		}
	}

private:
	uint32_t ts_;
	static uint32_t period_;
	static uint32_t lastEntry_;
};

#else /* #ifndef METARESTORE */

class ChecksumUpdater {
public:
	ChecksumUpdater(uint32_t) {
	}
};

#endif /* #ifndef METARESTORE */
