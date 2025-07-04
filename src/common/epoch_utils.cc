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

#include "epoch_utils.h"
#include <iomanip>
#include <sstream>
#include <time.h> // For gmtime_r and tm struct

namespace sauna_fs {
namespace epoch_utils {

std::string epoch_to_utc_string(uint64_t epoch_seconds) {
    time_t seconds = static_cast<time_t>(epoch_seconds);
    struct tm timeinfo;

    // Use gmtime_r for thread-safety
    if (gmtime_r(&seconds, &timeinfo) == nullptr) {
        // Handle error, though gmtime_r returning null is rare for valid timestamps
        return "ERR"; // Or throw an exception
    }

    std::ostringstream oss;
    // Format: YYYY-MM-DDTHHMMZ
    // struct tm members:
    // tm_year: years since 1900
    // tm_mon: months since January [0-11]
    // tm_mday: day of the month [1-31]
    // tm_hour: hours since midnight [0-23]
    // tm_min: minutes after the hour [0-59]
    // tm_sec: seconds after the minute [0-60] (60 for leap second)

    oss << std::setfill('0')
        << (timeinfo.tm_year + 1900)
        << "-"
        << std::setw(2) << (timeinfo.tm_mon + 1)
        << "-"
        << std::setw(2) << timeinfo.tm_mday
        << "T"
        << std::setw(2) << timeinfo.tm_hour
        << std::setw(2) << timeinfo.tm_min
        << "Z";

    return oss.str();
}

} // namespace epoch_utils
} // namespace sauna_fs
