include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(COMMON)

if(MINGW)
list(REMOVE_ITEM COMMON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/lockfile.cc")
list(REMOVE_ITEM COMMON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/memory_mapped_file.cc")
list(REMOVE_ITEM COMMON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/metadata.cc")
list(REMOVE_ITEM COMMON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/scoped_timer.cc")
list(REMOVE_ITEM COMMON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/setup.cc")
list(REMOVE_ITEM COMMON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/loop_watchdog.cc")
endif()

if(ISAL_LIBRARY)
  list(REMOVE_ITEM COMMON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/galois_field_isal.cc")
  list(REMOVE_ITEM COMMON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/galois_field_encode.cc")
endif()


shared_add_library(leil-common ${COMMON_SOURCES})
shared_target_link_libraries(leil-common STATIC sfserr SHARED sfserr_pic)
shared_target_link_libraries(leil-common STATIC slogger SHARED slogger_pic)
# Backward-compat aliases for the rename window (sfscommon -> leil-common).
# Lets the plugins and satellite repos keep linking against the legacy target
# name until they are updated in lockstep.
add_library(sfscommon ALIAS leil-common)
if(ENABLE_PIC_TARGETS)
  add_library(sfscommon_pic ALIAS leil-common_pic)
endif()

# ---- OpenSSL library ----
shared_target_link_libraries(leil-common OpenSSL::SSL OpenSSL::Crypto)

if (HAVE_CRCUTIL)
  # Use crcutil Library
  include_directories(SYSTEM ${CRCUTIL_INCLUDE_DIRS}) # skip compiler warnings for external crcutil headers
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CRCUTIL_CXX_FLAGS}")
  shared_target_link_libraries(leil-common ${CRCUTIL_LIBRARIES})
endif()
if (ZLIB_FOUND)
  shared_target_link_libraries(leil-common ${ZLIB_LIBRARIES})
endif()
if (RT_LIBRARY)
  shared_target_link_libraries(leil-common ${RT_LIBRARY})
endif()
if(TCMALLOC_LIBRARY AND ENABLE_TCMALLOC)
  message(STATUS "tcmalloc found, using it from: ${TCMALLOC_LIBRARY}")
  shared_target_link_libraries(leil-common ${TCMALLOC_LIBRARY})
endif()
if(JEMALLOC_LIBRARY AND ENABLE_JEMALLOC)
  message(STATUS "jemalloc found, using it from: ${JEMALLOC_LIBRARY}")
  shared_target_link_libraries(leil-common ${JEMALLOC_LIBRARY})
endif()
if(SOCKET_FOUND)
  shared_target_link_libraries(leil-common ${SOCKET_LIBRARIES})
endif()
if(ISAL_LIBRARY)
  shared_target_link_libraries(leil-common STATIC ${ISAL_LIBRARY} SHARED ${ISAL_PIC_LIBRARY})
endif()
if(JUDY_LIBRARY)
  shared_target_link_libraries(leil-common ${JUDY_LIBRARY})
endif()

create_unittest(leil-common ${COMMON_TESTS} ../master/goal_config_loader.cc)
link_unittest(leil-common leil-common)

set(PAM_D_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/pam.d")
install(DIRECTORY DESTINATION "${PAM_D_DIR}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/saunafs" "session required pam_limits.so")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/saunafs" DESTINATION "${PAM_D_DIR}")
