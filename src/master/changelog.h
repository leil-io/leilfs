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

constexpr uint32_t kMaxLogLineSize = 200000;

/// Initializes changelog module.
/// \param base_data_path_val Path to the directory where changelogs are stored.
/// \param cluster_id_val The cluster ID.
/// \param hostname_val The local hostname.
/// \param base_log_name_val Base name of the log file (e.g., "changelog.sfs" or "changelog_ml.sfs").
/// \param is_metalogger_val True if this is a metalogger.
/// \param min_back_logs_val Minimum allowed value of BACK_LOGS config entry.
/// \param max_back_logs_val Maximum allowed value of BACK_LOGS config entry.
/// \throws InitializeException
void changelog_init(
    const std::string& base_data_path_val,
    const std::string& cluster_id_val,
    const std::string& hostname_val,
    const std::string& base_log_name_val,
    bool is_metalogger_val,
    uint32_t min_back_logs_val,
    uint32_t max_back_logs_val);

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
