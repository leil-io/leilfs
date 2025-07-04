/*
   Copyright 2023 Leil Storage OÜ

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

#include <cstdint>
#include <string>

namespace sauna_fs {
namespace epoch_utils {

/// Converts an epoch timestamp (seconds since epoch) to a UTC string.
/// The format is YYYY-MM-DDTHHMMZ as specified for changelog filenames.
/// Example: 1748985806 -> "2025-06-25T1423Z" (assuming 2025-06-25 14:23:26 UTC)
/// Note: The 'seconds' part of the epoch time is truncated for HHMM format.
std::string epoch_to_utc_string(uint64_t epoch_seconds);

} // namespace epoch_utils
} // namespace sauna_fs
