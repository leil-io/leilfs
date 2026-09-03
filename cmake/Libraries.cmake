include(DownloadExternal)

# Find GoogleTest
if(ENABLE_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
endif()

# Find fmt and spdlog
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# Find standard libraries
find_package(Socket REQUIRED)
find_package(Threads REQUIRED)

# Find yaml-cpp
find_package(yaml-cpp CONFIG REQUIRED)

# Find FUSE3
if(NOT MINGW)
  find_package(FUSE3)
  if(NOT (FUSE3_FOUND))
    message(FATAL_ERROR "Could not find FUSE library (required)")
  endif()
endif()

# Find rt
find_library(RT_LIBRARY rt)
message(STATUS "RT_LIBRARY: ${RT_LIBRARY}")

# Find JEMALLOC or TCMALLOC
if(ENABLE_TCMALLOC AND ENABLE_JEMALLOC)
    message(FATAL_ERROR "You cannot enable both TCMALLOC and JEMALLOC simultaneously")
endif()
if(ENABLE_TCMALLOC)
  find_library(TCMALLOC_LIBRARY NAMES tcmalloc_minimal)
  message(STATUS "TCMALLOC_LIBRARY: ${TCMALLOC_LIBRARY}")
endif()
if(ENABLE_JEMALLOC)
  find_library(JEMALLOC_LIBRARY NAMES jemalloc)
  message(STATUS "JEMALLOC_LIBRARY: ${JEMALLOC_LIBRARY}")
endif()

# Find extra binaries
if(NOT WIN32)
  include(FindAsciidoctor)
endif()

# Find Zlib
find_package(ZLIB)
if(ZLIB_FOUND)
  message(STATUS "Found Zlib ${ZLIB_VERSION_STRING}")
  set(SAUNAFS_HAVE_ZLIB_H 1)
else()
  message(STATUS "Could not find Zlib")
  message(STATUS "   This dependency is optional.")
  message(STATUS "   If it's installed in a non-standard path, set ZLIB_ROOT variable")
  message(STATUS "   to point this path (cmake -DZLIB_ROOT=...)")
endif()

# Find Systemd
INCLUDE(FindPkgConfig)
pkg_check_modules(SYSTEMD libsystemd)
if(SYSTEMD_FOUND)
  check_include_files(systemd/sd-daemon.h SAUNAFS_HAVE_SYSTEMD_SD_DAEMON_H)
  message(STATUS "Found Systemd ${SYSTEMD_VERSION_STRING}")
else()
  message(STATUS "Could not find Systemd (but it is not required)")
endif()

# Find Boost
find_package(Boost CONFIG REQUIRED COMPONENTS filesystem iostreams program_options)

# Find crcutil
if(NOT BIG_ENDIAN)
  INCLUDE(FindPkgConfig)
  pkg_check_modules(CRCUTIL libcrcutil)
  if(CRCUTIL_FOUND)
    message(STATUS "Found libcrcutil")
    set(HAVE_CRCUTIL 1)
  else()
    message(STATUS "Could NOT find system libcrcutil (but it's not required)")
    set(CRCUTIL_VERSION crcutil-1.0)
    message(STATUS "Using bundled ${CRCUTIL_VERSION}")
    set(HAVE_CRCUTIL 1)
    set(CRCUTIL_LIBRARIES "crcutil")
    set(CRCUTIL_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/external/${CRCUTIL_VERSION}/code)
    set(CRCUTIL_SOURCE_DIR ${CMAKE_SOURCE_DIR}/external/${CRCUTIL_VERSION}/code)

    if(CXX_HAS_MCRC32)
      set(CRCUTIL_CXX_FLAGS "-mcrc32")
    else()
      set(CRCUTIL_CXX_FLAGS "")
    endif()

  endif()
endif()


# Find Judy
find_package(Judy)
if(JUDY_FOUND)
  set(SAUNAFS_HAVE_JUDY YES)
  set(SAUNAFS_HAVE_WORKING_JUDY1 ${JUDY_HAVE_WORKING_JUDY1})
endif()

# Find PAM libraries
find_package(PAM)
if(PAM_FOUND)
  set(SAUNAFS_HAVE_PAM YES)
endif()

# Find BerkeleyDB
find_package(DB 11.2.5.2)

# Find Intel Storage Acceleration library
find_library(ISAL_LIBRARY isal)
if(APPLE)
  find_library(ISAL_PIC_LIBRARY libisal.dylib)
else()
  find_library(ISAL_PIC_LIBRARY libisal.so)
endif()
if(NOT ISAL_PIC_LIBRARY)
  find_library(ISAL_PIC_LIBRARY isal_pic)
endif()
if (NOT ISAL_PIC_LIBRARY)
  message(WARNING "Some systems may require position-independent ISA-L library.")
endif()
message(STATUS "ISAL(Intel Storage Acceleration) LIBRARY: ${ISAL_LIBRARY}")
message(STATUS "ISAL PIC LIBRARY: ${ISAL_PIC_LIBRARY}")

# Download nfs-ganesha
if(ENABLE_NFS_GANESHA)
  # Single source of truth for the Ganesha release: tests/ci_build/ganesha/ganesha.env
  # (shared with the CI workflow and the Ganesha Docker image).
  set(GANESHA_ENV "${CMAKE_SOURCE_DIR}/tests/ci_build/ganesha/ganesha.env")
  file(STRINGS "${GANESHA_ENV}" _ganesha_version_line REGEX "^GANESHA_VERSION=")
  string(REGEX REPLACE "^GANESHA_VERSION=" "" GANESHA_GIT_TAG "${_ganesha_version_line}")
  file(STRINGS "${GANESHA_ENV}" _ganesha_url_line REGEX "^GANESHA_GIT_URL=")
  string(REGEX REPLACE "^GANESHA_GIT_URL=" "" GANESHA_GIT_URL "${_ganesha_url_line}")
  # Local directory name keeps the numeric form (V9.15 -> nfs-ganesha-9.15).
  string(REGEX REPLACE "^[Vv]" "" NFS_GANESHA_VERSION "${GANESHA_GIT_TAG}")

  # Clone at the pinned tag WITH submodules so src/libntirpc is checked out at the
  # exact commit this release pins. ntirpc thus tracks the Ganesha version with no
  # separate pin to maintain (a source zip would omit the submodule entirely).
  # CI checks this directory out beforehand with its GitHub credential (Jenkinsfile,
  # fetchGanesha), so on Jenkins the clone below finds it and is skipped.
  clone_external_git(NFS_GANESHA "nfs-ganesha-${NFS_GANESHA_VERSION}"
                     "${GANESHA_GIT_URL}" "${GANESHA_GIT_TAG}")
  # ntirpc headers live inside the Ganesha submodule checkout.
  set(NTIRPC_DIR_NAME "${NFS_GANESHA_DIR_NAME}/src/libntirpc" CACHE INTERNAL "" FORCE)
endif()

# Find Prometheus
find_package(prometheus-cpp CONFIG)
if (PROMETHEUS_CPP_ENABLE_PULL)
    message(STATUS "Found Prometheus C++ Client Library")
else()
    message(STATUS "Did not find Prometheus C++ Client Library (but not needed)")
endif()

# Find OpenSSL
find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL: includes=${OPENSSL_INCLUDE_DIR}, libs=${OPENSSL_LIBRARIES}")
