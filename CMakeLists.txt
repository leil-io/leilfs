cmake_minimum_required(VERSION 3.20)

project(saunafs LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Enable symbol export for loadable modules, ensuring same shared resource
# references. Needed for the plugins, as they are shared libraries.
set(CMAKE_ENABLE_EXPORTS ON)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "In-source builds are disabled for this project. This is for your "
    "safety. To build the project, please create a build tree in "
    "a separate directory, for example:\n"
    "     mkdir ${CMAKE_SOURCE_DIR}/build\n"
    "     cd ${CMAKE_SOURCE_DIR}/build\n"
    "     cmake ..\n"
    "Before doing that you may need to clean up after this try by removing "
    "CMakeCache.txt file and CMakeFiles/ directory.\n"
    "If you REALLY need an in-source build, remove this check from "
    "${CMAKE_CURRENT_LIST_FILE} and try again."
  )
endif()

option(ENABLE_WERROR            "Enable treating compilation warnings as errors"         OFF)
option(ENABLE_LIGHTSFS          "Enable light version of SaunaFS"                       OFF)
option(ENABLE_UTILS             "Enable building additional binaries used e.g. in tests" OFF)
option(ENABLE_TESTS             "Enable building unit and functional tests"              OFF)
option(ENABLE_DOCS              "Enable building the documentation"                      ON)
option(ENABLE_EXIT_ON_USR1      "Enable handler for SIGUSR1 which calls exit()"          OFF)
option(THROW_INSTEAD_OF_ABORT   "Throw std::exception instead of calling abort"          OFF)
option(ENABLE_DEBUG_LOG         "Enable prefix based debug logs"                         OFF)
option(ENABLE_URAFT             "Enable installation of uraft util"                      ON)
option(ENABLE_VERBOSE_ASCIIDOCTOR  "Enable verbose output of asciidoctor"                OFF)
option(ENABLE_TCMALLOC          "Enable use of tcmalloc-minimal library"                 OFF)
option(ENABLE_JEMALLOC          "Enable use of jemalloc library"                         OFF)
option(ENABLE_CLIENT_LIB        "Enable dynamic client library (libsaunafs-client)"     OFF)
# end of SaunaFS options
option(ENABLE_TRACES            "Enable traces"                                          OFF)
option(ENABLE_SYSLOG_FOR_TRACES "Enable usage of syslog instead of stdout for traces"    OFF)
option(ENABLE_CRC               "Enable checksums"                                       ON)
option(ENABLE_REQUEST_LOG       "Enable logging request times"                           OFF)
option(USE_LEGACY_READ_MESSAGES "Enable sending old type of messages by mount"           OFF)
option(ENABLE_NFS_GANESHA       "Enable nfs-ganesha plugin"                              OFF)
option(ENABLE_CCACHE            "Enable ccache during compilation"                       ON)
option(ENABLE_NFS_ACL_SUPPORT   "Enable nfs-ganesha ACL support"                         ON)
option(ENABLE_COMPILE_COMMANDS  "Enable generation of compile_commands.json"             ON)
option(ASCIIDOCTOR_AUTO_SETUP   "Permit CMake to self-install asciidoctor gem when absent" ON)
# Experimental
option(ENABLE_INODE64           "Enable 64 bits for inode numbers and quantities"        OFF)
option(ENABLE_FOUNDATIONDB      "Enable FoundationDB experimental support"               OFF)
option(GENERATE_GIT_INFO "Generate git info from current repository (for version commands)" ON)

# If enabled (default), the file compile_commands.json is generated in the build
# directory. Tools like clangd and vscode use this file for proper analysis.
set(CMAKE_EXPORT_COMPILE_COMMANDS ${ENABLE_COMPILE_COMMANDS})

find_program(CCACHE_FOUND ccache)
message(STATUS "ENABLE_CCACHE: ${ENABLE_CCACHE}")
message(STATUS "CCACHE_FOUND: ${CCACHE_FOUND}")

if(ENABLE_CCACHE AND CCACHE_FOUND)
  set(CMAKE_C_COMPILER_LAUNCHER ccache)
  set(CMAKE_CXX_COMPILER_LAUNCHER ccache)
  message(STATUS "Using ccache")
endif()

set(DEFAULT_MIN_VERSION "5.13.0")

include(GNUInstallDirs)

set(BIN_SUBDIR                  "${CMAKE_INSTALL_FULL_BINDIR}")
set(SBIN_SUBDIR                 "${CMAKE_INSTALL_FULL_SBINDIR}")
set(LIB_SUBDIR                  "${CMAKE_INSTALL_FULL_LIBDIR}")
set(MAN_SUBDIR                  "${CMAKE_INSTALL_FULL_MANDIR}")
set(CGI_SUBDIR                  "${CMAKE_INSTALL_FULL_DATAROOTDIR}/leil-cgi")
set(INCL_SUBDIR                 "${CMAKE_INSTALL_FULL_INCLUDEDIR}/saunafs")

set(ETC_SUBDIR                  "${CMAKE_INSTALL_FULL_SYSCONFDIR}/saunafs")
set(RUN_SUBDIR                  "${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/run/saunafs")
set(DATA_SUBDIR                 "${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/lib/saunafs")
set(USR_SHARE_DOC               "${CMAKE_INSTALL_FULL_DATAROOTDIR}/doc/")
set(EXAMPLES_SUBDIR             "examples")
set(CHUNKSERVER_NAME            "saunafs-chunkserver")
set(CLIENT_NAME                 "saunafs-client")
set(METALOGGER_NAME             "saunafs-metalogger")
set(MASTER_NAME                 "saunafs-master")
set(SFSMDS_NAME                 "saunafs-mds")
set(URAFT_NAME                  "saunafs-uraft")
string(REGEX REPLACE "/$" "" USR_SHARE_DOC_NO_SLASH "${USR_SHARE_DOC}")
set(CHUNKSERVER_EXAMPLES_SUBDIR "${USR_SHARE_DOC_NO_SLASH}/${CHUNKSERVER_NAME}/${EXAMPLES_SUBDIR}")
set(CLIENT_EXAMPLES_SUBDIR      "${USR_SHARE_DOC_NO_SLASH}/${CLIENT_NAME}/${EXAMPLES_SUBDIR}")
set(METALOGGER_EXAMPLES_SUBDIR  "${USR_SHARE_DOC_NO_SLASH}/${METALOGGER_NAME}/${EXAMPLES_SUBDIR}")
set(SFSMASTER_EXAMPLES_SUBDIR   "${USR_SHARE_DOC_NO_SLASH}/${MASTER_NAME}/${EXAMPLES_SUBDIR}")
set(SFSMDS_EXAMPLES_SUBDIR      "${USR_SHARE_DOC_NO_SLASH}/${SFSMDS_NAME}/${EXAMPLES_SUBDIR}")
set(URAFT_EXAMPLES_SUBDIR       "${USR_SHARE_DOC_NO_SLASH}/${URAFT_NAME}/${EXAMPLES_SUBDIR}")

# Aliases
set(SBIN_PATH                   "${SBIN_SUBDIR}")
set(LIB_PATH                    "${LIB_SUBDIR}")
set(PLUGINS_PATH                "${LIB_PATH}/saunafs/plugins")
set(BUILD_PATH                  "${CMAKE_BINARY_DIR}")
set(CGI_PATH                    "${CGI_SUBDIR}")
set(ETC_PATH                    "${ETC_SUBDIR}")
set(DATA_PATH                   "${DATA_SUBDIR}")

set(DEFAULT_USER                 "saunafs"                 CACHE STRING "Default user to run daemons as")
set(DEFAULT_GROUP                "saunafs"                 CACHE STRING "Default group to run daemons as")
set(DEFAULT_MASTER_HOSTNAME      "sfsmaster"                CACHE STRING "Default master server hostname")
set(DEFAULT_MASTER_PORT          "9421"                     CACHE STRING "Default master server port number")
set(DEFAULT_MOUNTED_SUBFOLDER    "/"                        CACHE STRING "Default subfolder to be mounted")
set(DEFAULT_SFSMOUNT_CONFIG_PATH "${ETC_PATH}/leil-mount.cfg" CACHE STRING "Default full path to leil-mount configuration file")
set(SAUNAFS_BLOCKS_IN_CHUNK     "1024"                     CACHE STRING "Number of blocks in one chunk")
set(SAUNAFS_BLOCK_SIZE          "65536"                    CACHE STRING "Number of bytes in one block")
set(ASCIIDOCTOR_VERSION         "2.0.23"                   CACHE STRING "Pinned asciidoctor gem version")

message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")
message(STATUS "CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "DEFAULT_USER: ${DEFAULT_USER}")
message(STATUS "DEFAULT_GROUP: ${DEFAULT_GROUP}")
message(STATUS "SAUNAFS_BLOCKS_IN_CHUNK: ${SAUNAFS_BLOCKS_IN_CHUNK}")
message(STATUS "SAUNAFS_BLOCK_SIZE: ${SAUNAFS_BLOCK_SIZE}")
message(STATUS "ENABLE_WERROR: ${ENABLE_WERROR}")
message(STATUS "ENABLE_LIGHTSFS: ${ENABLE_LIGHTSFS}")
message(STATUS "ENABLE_UTILS: ${ENABLE_UTILS}")
message(STATUS "ENABLE_TESTS: ${ENABLE_TESTS}")
message(STATUS "ENABLE_DOCS: ${ENABLE_DOCS}")
message(STATUS "ENABLE_EXIT_ON_USR1: ${ENABLE_EXIT_ON_USR1}")
message(STATUS "THROW_INSTEAD_OF_ABORT: ${THROW_INSTEAD_OF_ABORT}")
message(STATUS "ENABLE_DEBUG_LOG: ${ENABLE_DEBUG_LOG}")
message(STATUS "ENABLE_VERBOSE_ASCIIDOCTOR: ${ENABLE_VERBOSE_ASCIIDOCTOR}")
message(STATUS "ENABLE_TCMALLOC: ${ENABLE_TCMALLOC}")
message(STATUS "ENABLE_JEMALLOC: ${ENABLE_JEMALLOC}")
message(STATUS "ENABLE_CLIENT_LIB: ${ENABLE_CLIENT_LIB}")
message(STATUS "ENABLE_URAFT: ${ENABLE_URAFT}")
# end of SaunaFS options values
message(STATUS "ENABLE_TRACES: ${ENABLE_TRACES}")
message(STATUS "ENABLE_SYSLOG_FOR_TRACES: ${ENABLE_SYSLOG_FOR_TRACES}")
message(STATUS "ENABLE_CRC: ${ENABLE_CRC}")
message(STATUS "ENABLE_REQUEST_LOG: ${ENABLE_REQUEST_LOG}")
message(STATUS "USE_LEGACY_READ_MESSAGES: ${USE_LEGACY_READ_MESSAGES}")
message(STATUS "ENABLE_NFS_GANESHA: ${ENABLE_NFS_GANESHA}")
message(STATUS "ENABLE_COMPILE_COMMANDS: ${CMAKE_EXPORT_COMPILE_COMMANDS}")
message(STATUS "ENABLE_INODE64: ${ENABLE_INODE64}")
message(STATUS "ENABLE_FOUNDATIONDB: ${ENABLE_FOUNDATIONDB}")

if(ENABLE_INODE64)
  add_definitions(-DSAUNAFS_USE_INODE64)
endif()

if(ENABLE_TESTS)
  if(NOT THROW_INSTEAD_OF_ABORT)
    message(STATUS "Tests require THROW_INSTEAD_OF_ABORT to be set to YES, changing passed value:")
    set(THROW_INSTEAD_OF_ABORT YES)
    message(STATUS "THROW_INSTEAD_OF_ABORT: ${THROW_INSTEAD_OF_ABORT}")
  endif()
  if(NOT ENABLE_CLIENT_LIB)
    message(STATUS "Tests require ENABLE_CLIENT_LIB to be set to YES, changing passed value:")
    set(ENABLE_CLIENT_LIB YES)
    message(STATUS "ENABLE_CLIENT_LIB: ${ENABLE_CLIENT_LIB}")
  endif()
  set(BUILD_TESTS ON)
  set(BUILD_UTILS ON)
  set(SAUNAFS_ENABLE_DEBUG_LOG ON)
endif()

if(ENABLE_CLIENT_LIB)
  set(ENABLE_PIC_TARGETS ON)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "coverage" OR CODE_COVERAGE)
  if("${CMAKE_C_COMPILER_ID}" MATCHES "(Apple)?[Cc]lang" OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "(Apple)?[Cc]lang")
    message("Building with llvm Code Coverage Tools")
    find_program(LLVM_COV_PATH llvm-cov)
    if(NOT LLVM_COV_PATH)
      message(FATAL_ERROR "llvm-cov not found! Aborting.")
    endif()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
  elseif("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU")
    message("Building with lcov Code Coverage Tools")
    if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug"))
      message(WARNING "Code coverage results with an optimized (non-Debug) build may be misleading")
    endif()
    find_program(LCOV_PATH NAMES lcov lcov.bat lcov.exe lcov.perl)
    if(NOT LCOV_PATH)
      message(FATAL_ERROR "lcov not found! Aborting...")
    endif()
    find_program(GENHTML_PATH NAMES genhtml genhtml.perl genhtml.bat)
    if(NOT GENHTML_PATH)
      message(FATAL_ERROR "genhtml not found! Aborting...")
    endif()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage -fprofile-arcs -ftest-coverage")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -fprofile-arcs -ftest-coverage")
  else()
    message(FATAL_ERROR "Code coverage requires Clang or GCC. Aborting.")
  endif()
endif()

string(TIMESTAMP BUILD_TIMESTAMP)
add_compile_definitions(BUILD_TIME="${BUILD_TIMESTAMP}")

if(DEFINED VERSION_SUFFIX)
  set(PACKAGE_VERSION "${DEFAULT_MIN_VERSION}${VERSION_SUFFIX}")
else()
  set(PACKAGE_VERSION "${DEFAULT_MIN_VERSION}-dev")
endif()

string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" PACKAGE_VERSION_MATCH ${PACKAGE_VERSION})
set(PACKAGE_VERSION_MAJOR ${CMAKE_MATCH_1})
set(PACKAGE_VERSION_MINOR ${CMAKE_MATCH_2})
set(PACKAGE_VERSION_MICRO ${CMAKE_MATCH_3})
message(STATUS "PACKAGE_VERSION: ${PACKAGE_VERSION}")
message(STATUS "PACKAGE_VERSION_MAJOR: ${PACKAGE_VERSION_MAJOR}")
message(STATUS "PACKAGE_VERSION_MINOR: ${PACKAGE_VERSION_MINOR}")
message(STATUS "PACKAGE_VERSION_MICRO: ${PACKAGE_VERSION_MICRO}")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m64" )
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -m64")
endif()

if(GIT_COMMIT)
  add_compile_definitions(GIT_COMMIT="${GIT_COMMIT}")
elseif(GENERATE_GIT_INFO)
  execute_process(COMMAND "git" "rev-parse" "HEAD"
  OUTPUT_VARIABLE GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE)
  add_compile_definitions(GIT_COMMIT="${GIT_COMMIT}")
else()
  add_compile_definitions(GIT_COMMIT="N/A")
endif()

if(GIT_BRANCH)
  add_compile_definitions(GIT_BRANCH="${GIT_BRANCH}")
elseif(GENERATE_GIT_INFO)
  execute_process(COMMAND "git" "rev-parse" "--abbrev-ref" "HEAD"
  OUTPUT_VARIABLE GIT_BRANCH OUTPUT_STRIP_TRAILING_WHITESPACE)
  add_compile_definitions(GIT_BRANCH="${GIT_BRANCH}")
else()
  add_compile_definitions(GIT_BRANCH="N/A")
endif()

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  set(LINUX ON)
  set(UNIX ON)
endif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")

if (LINUX)
    set(PLATFORM "LINUX")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-strict-aliasing")
    find_library(LIBDL dl)  # module loader
endif(LINUX)

if(MINGW)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wformat=0")
endif()
string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pipe -pthread -Wall -Wextra -Wpedantic -fwrapv")
if(ENABLE_WERROR)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
endif()
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG -g")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -O2 -ftree-vectorize -g -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_COVERAGE "-O0 -g --coverage")
set(CMAKE_CXX_FLAGS_CLANG_ANALYZE "--analyze -Xanalyzer -analyzer-output=text")
set(CMAKE_CXX_FLAGS_CXXDEBUG "-O0 -g -D_GLIBCXX_DEBUG")

# Sanitizer support: configure with -DSANITIZE=address|thread|undefined (empty disables).
# Works with GCC and Clang. MemorySanitizer is not supported: it needs every dependency,
# including libc++, instrumented too.
set(SANITIZE "" CACHE STRING "Build with a sanitizer: address, thread, or undefined")
set_property(CACHE SANITIZE PROPERTY STRINGS "" address thread undefined)
set(SANITIZE_SUPPORTED_VALUES address thread undefined)
if(SANITIZE)
  if(SANITIZE STREQUAL "memory")
    message(FATAL_ERROR
      "SANITIZE=memory needs all dependencies and libc++ instrumented; not supported.")
  endif()
  if(NOT SANITIZE IN_LIST SANITIZE_SUPPORTED_VALUES)
    message(FATAL_ERROR
      "Unsupported SANITIZE value: '${SANITIZE}'. "
      "Expected address, thread, undefined, or an empty value.")
  endif()
  add_compile_options(-g -fno-omit-frame-pointer -fsanitize=${SANITIZE})
  add_link_options(-fsanitize=${SANITIZE})
  message(STATUS "Sanitizer enabled: SANITIZE=${SANITIZE}")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")

add_definitions(-D_LARGEFILE_SOURCE)
add_definitions(-D_FILE_OFFSET_BITS=64)
add_definitions(-DSAUNAFS_HAVE_CONFIG_H)
add_definitions(-D__STDC_LIMIT_MACROS)
add_definitions(-D__STDC_CONSTANT_MACROS)
add_definitions(-D__STDC_FORMAT_MACROS)
# Disable default stdout logger, enabling it breaks some features of SaunaFS
# Enable it on your own responsibility
add_definitions(-DSPDLOG_DISABLE_DEFAULT_LOGGER)
add_definitions(-DSPDLOG_FMT_EXTERNAL)
add_definitions(-DFMT_HEADER_ONLY)
if(MINGW)
  add_definitions(-DWINVER=0x0602)
  add_definitions(-D_WIN32_WINNT=0x0602)
  add_definitions(-DFD_SETSIZE=1024)
endif()

# All the environment tests (libs, includes, etc.) live here:
include(EnvTests)
include(Libraries)
include(CollectSources)
include(CreateUnitTest)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Qunused-arguments")
endif()

if(CXX_HAS_WNOGNU)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-gnu")
endif()

if(ENABLE_LIGHTSFS)
  set(PROTO_BASE 65536)
  set(LIGHT_SFS 1)
else()
  set(PROTO_BASE 0)
  set(LIGHT_SFS 0)
endif()

if(ENABLE_UTILS)
  set(BUILD_UTILS ON)
endif()

if(ENABLE_DEBUG_LOG)
  set(SAUNAFS_ENABLE_DEBUG_LOG ON)
endif()

if(NOT Boost_INCLUDE_DIRS)
  message(FATAL_ERROR "Boost headers are required")
endif()

if(ENABLE_TRACES)
  add_definitions(-DENABLE_TRACES)

  if (ENABLE_SYSLOG_FOR_TRACES)
    add_definitions(-DENABLE_SYSLOG_FOR_TRACES)
  endif()
endif()

if(ENABLE_REQUEST_LOG)
  if(NOT Boost_IOSTREAMS_FOUND)
    message(FATAL_ERROR "Boost iostreams is required if request time logging is enabled")
  endif()
  set(ADDITIONAL_LIBS ${Boost_IOSTREAMS_LIBRARY})
  add_definitions(-DENABLE_REQUEST_LOG)
endif()

set(CHARTS_CSV_CHARTID_BASE 90000)

if(THROW_INSTEAD_OF_ABORT)
  add_definitions(-DTHROW_INSTEAD_OF_ABORT)
endif()

if(USE_LEGACY_READ_MESSAGES)
  add_definitions(-DUSE_LEGACY_READ_MESSAGES)
endif()

if(ENABLE_CRC)
  message(STATUS "CRC checksums enabled")
  add_definitions(-DENABLE_CRC)
else()
  message(STATUS "CRC checksums disabled")
endif()

set(CHARTS_CSV_CHARTID_BASE 90000)

# Internal capability required by some FSALs
set(GSH_CAN_HOST_LOCAL_FS ${ENABLE_NFS_GANESHA})

# Create config.h file
configure_file(config.h.in config.h)
include_directories(${CMAKE_BINARY_DIR}) # Here we have generated config.h

# main.cc shared by some SaunaFS applications
set(MAIN_SRC ${CMAKE_SOURCE_DIR}/src/main/main.cc)

if(ZLIB_FOUND)
  include_directories(${ZLIB_INCLUDE_DIRS})
endif()
include_directories(${CMAKE_SOURCE_DIR}/src) # Allow paths relative to src, eg. #include common/crc.h
link_directories(${LINK_DIRECTORIES} ${FUSE_LIBRARY_DIR})

set(ICON_RESOURCE_FILE "")
if(MINGW)
  option(ENABLE_WINFSP_WINCLIENT  "Enable winfsp library based windows client build"    ON)

  if(ENABLE_WINFSP_WINCLIENT)
    set(WINFSP_FUSE3_INCLUDE_DIR  "C:/Program Files (x86)/WinFsp/inc/fuse3"             CACHE STRING "winfsp installation /inc directory")
    set(WINFSP_FUSE_DLL  "C:/Program Files (x86)/WinFsp/bin/winfsp-x64.dll"    CACHE STRING "winfsp-fuse dll for specific arch")
    set(WINFSP_FUSE_LIBRARY  "C:/Program Files (x86)/WinFsp/lib/winfsp-x64.lib"    CACHE STRING "winfsp-fuse lib for specific arch")
  endif()

  set(ICON_RESOURCE_FILE "${CMAKE_SOURCE_DIR}/src/mount/windows/resources/icons/icon.rc")
endif()

add_subdirectory(external)
add_subdirectory(src/admin)
add_subdirectory(src/config)
add_subdirectory(src/errors)
add_subdirectory(src/slogger)
add_subdirectory(src/metrics)
add_subdirectory(src/common)
add_subdirectory(src/protocol)
add_subdirectory(src/tools)
add_subdirectory(src/mount)

if(NOT MINGW)
  if(BUILD_DEVTOOLS)
    add_subdirectory(src/devtools)
  endif()
  add_subdirectory(src/chunkserver)
  add_subdirectory(src/kv)
  if(ENABLE_FOUNDATIONDB)
    add_subdirectory(src/fdb)
  endif()
  add_subdirectory(src/master)
  if(ENABLE_FOUNDATIONDB AND EXISTS ${CMAKE_SOURCE_DIR}/src/mds)
    ## mds does not make sense so far without fdb
    add_subdirectory(src/mds)
  endif()
  add_subdirectory(src/metadump)
  add_subdirectory(src/metalogger)
  add_subdirectory(src/metarestore)
  if (FUSE_LIBRARY OR FUSE3_LIBRARY)
    add_subdirectory(src/mount/fuse)
  endif()
  add_subdirectory(src/data)
  add_subdirectory(src/cgi)
  if(BUILD_UTILS)
    add_subdirectory(utils)
  endif()
  if(ENABLE_DOCS)
    add_subdirectory(doc)
  endif()
  if(ENABLE_NFS_GANESHA)
    add_subdirectory(src/nfs-ganesha)
  endif()
endif()

if (ENABLE_URAFT)
  add_subdirectory(src/uraft)
endif()

# 'src/unittests' should be the last to be added to guarantee that all unit
# tests are included.
# Otherwise, some unit tests could not be generated, like uraft's ones.
if(BUILD_TESTS)
  add_subdirectory(src/unittests)
  add_subdirectory(tests)
endif()

include(Packing)

include(GoogleTest)
