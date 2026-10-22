#  Copyright 2017 Skytechnology sp. z o.o.
#  Copyright 2023      Leil Storage OÜ
#
#  This file is part of LeilFS.
#
#  LeilFS is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, version 3.
#
#  LeilFS is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with LeilFS  If not, see <http://www.gnu.org/licenses/>.

collect_sources(NFS_GANESHA_PLUGIN)

add_definitions(-D__USE_MISC)

include_directories(${CMAKE_SOURCE_DIR}/external/${NFS_GANESHA_DIR_NAME}/src/include)
include_directories(${CMAKE_SOURCE_DIR}/external/${NTIRPC_DIR_NAME}/ntirpc)
include_directories(${CMAKE_SOURCE_DIR}/external/${NTIRPC_DIR_NAME}/src/monitoring/include)

add_library(fsalsaunafs MODULE ${NFS_GANESHA_PLUGIN_MAIN} ${NFS_GANESHA_PLUGIN_SOURCES})

target_include_directories(fsalsaunafs PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        )

target_link_libraries(fsalsaunafs leil-client_pic)

# Ganesha dlopen()s FSALs with RTLD_DEEPBIND, so malloc() resolves through the
# FSAL's own dependencies. Link the allocator Ganesha uses (jemalloc by default)
# so gsh_malloc() and Ganesha's malloc_usable_size()-based mem stats agree.
find_library(JEMALLOC_LIBRARY jemalloc)
if(JEMALLOC_LIBRARY)
  target_link_libraries(fsalsaunafs ${JEMALLOC_LIBRARY})
else()
  message(WARNING "jemalloc not found: the SaunaFS FSAL will crash in a jemalloc-based Ganesha")
endif()

# SaunaFS FSAL (libfsalsaunafs.so) will be installed at ${SAUNAFS_ROOT}/lib/ganesha
install(TARGETS fsalsaunafs COMPONENT fsal DESTINATION ${LIB_SUBDIR}/ganesha)
