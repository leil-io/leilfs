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

#include <cstdint>
#include <string>

#include "common/observable_property.h"

constexpr uint32_t kMaxLogLineSize = 200000;

struct ChangelogEvent {
	uint64_t version;
	std::string entry;
};

/// Accessor for the global changelog signal to avoid the warning about non-const global
Signal<const ChangelogEvent &> &getChangelogSignal();

/// Initializes changelog module.
/// \param changelogFilename - base name of changelog files, e.g. "changelog_ml.sfs"
/// \param minBackLogsNumber - minimum allowed value of BACK_LOGS config entry
/// \param maxBackLogsNumber - maximum allowed value of BACK_LOGS config entry
/// \throws InitializeException
void changelog_init(std::string changelogFilename,
		uint32_t minBackLogsNumber, uint32_t maxBackLogsNumber);

/// Return the value of \p BACK_LOGS config entry
uint32_t changelog_get_back_logs_config_value();

/// Rotates all the changelogs
void changelog_rotate();

/// Stores a new change
/// Format of the entry: <ts>|<COMMAND>(arg1,arg2,...)
void changelog(uint64_t version, const char* entry);

/// Flushes (fflush) the current changelog
void changelog_flush();

/// Disables flushing the current changelog after each \p changelog call
void changelog_disable_flush();

/// Enables flushing the current changelog after each \p changelog call
void changelog_enable_flush();

#ifndef METALOGGER
/// Returns version of the first entry in a changelog.
/// @param file -- path to the changelog file
/// @return 0 in case of any error.
uint64_t changelogGetFirstLogVersion(const std::string &fname);

/// Returns version of the last entry in a changelog.
/// @param file -- path to the changelog file
/// @return 0 in case of any error.
uint64_t changelogGetLastLogVersion(const std::string &fname);
#else   // #ifndef METALOGGER
uint64_t findLastLogVersion();
#endif  // #ifndef METALOGGER

#if !defined(METARESTORE) && !defined(METALOGGER)
/// Load and apply changelogs.
void load_changelogs();

/// Load and apply given changelog file.
void load_changelog(const std::string &path);
#endif  // !defined(METARESTORE) && !defined(METALOGGER)
