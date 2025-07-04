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

#include "system_utils.h"
#include <unistd.h> // For gethostname
#include <vector>   // For buffer

// Define HOST_NAME_MAX if not defined (e.g. on some systems)
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255 // POSIX standard is 255 for host names
#endif


namespace sauna_fs {
namespace system_utils {

std::string get_local_hostname() {
    std::vector<char> hostname_buffer(HOST_NAME_MAX + 1, 0);
    if (gethostname(hostname_buffer.data(), hostname_buffer.size() -1) == 0) {
        return std::string(hostname_buffer.data());
    }
    // On error, return empty string or handle error as appropriate
    // (e.g., log an error message)
    return "";
}

} // namespace system_utils
} // namespace sauna_fs
