#  Copyright 2015-2017 Skytechnology sp. z o.o.
#  Copyright 2023      Leil Storage OÜ
#
#  This file is part of SaunaFS.
#
#  SaunaFS is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, version 3.
#
#  SaunaFS is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(MOUNT)

shared_add_library(mount ${MOUNT_SOURCES})
shared_target_link_libraries(mount sfscommon ${ADDITIONAL_LIBS})
shared_target_set_properties(mount UNITY_BUILD "OFF" UNITY_BUILD_UNIQUE_ID "CLIENT_UNITY")

create_unittest(mount ${MOUNT_TESTS})
link_unittest(mount mount sfscommon)

if (ENABLE_CLIENT_LIB)
  add_subdirectory(client)
endif()

if(MINGW)
  add_subdirectory(windows)
endif()
