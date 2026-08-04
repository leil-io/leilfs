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
collect_sources(METRICS)

add_library(metrics ${METRICS_SOURCES})

target_link_libraries(metrics slogger)
if (ENABLE_PROMETHEUS AND PROMETHEUS_CPP_ENABLE_PULL)
  target_link_libraries(metrics prometheus-cpp::pull)
endif()
