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
#include "gtest/gtest.h"

namespace sauna_fs {
namespace system_utils {

TEST(SystemUtilsTest, GetHostnameReturnsNonEmpty) {
    std::string hostname = get_local_hostname();
    // We can't know what the hostname will be, but it shouldn't be empty
    // on a properly configured system.
    EXPECT_FALSE(hostname.empty()) << "get_local_hostname() returned an empty string.";
}

TEST(SystemUtilsTest, GetHostnameLength) {
    std::string hostname = get_local_hostname();
    if (!hostname.empty()) {
        // HOST_NAME_MAX is often 255, but can be 64 on older systems.
        // The buffer in get_local_hostname is HOST_NAME_MAX + 1.
        // The returned string length should be <= HOST_NAME_MAX.
        #ifndef HOST_NAME_MAX
        #define HOST_NAME_MAX 255
        #endif
        EXPECT_LE(hostname.length(), HOST_NAME_MAX) << "Hostname length exceeds HOST_NAME_MAX.";
    } else {
        // If hostname is empty (which might happen in some CI environments or minimal containers),
        // we just note it. The previous test handles the "empty" case.
        SUCCEED() << "Hostname is empty, length test skipped.";
    }
}

} // namespace system_utils
} // namespace sauna_fs
