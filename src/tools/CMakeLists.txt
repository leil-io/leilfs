include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(TOOLS)

set(TOOLS_RC_FILE "")
if(MINGW)
  include(WindowsVersion)
  sfs_configure_version_rc(
    "${CMAKE_CURRENT_BINARY_DIR}/saunafs-version.rc"
    "SaunaFS Tools"
    "Tools for SaunaFS"
    "saunafs"
    "saunafs.exe"
  )
  set(TOOLS_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/saunafs-version.rc")
endif()

if(ICON_RESOURCE_FILE STREQUAL "")
  add_executable(saunafs ${TOOLS_MAIN} ${TOOLS_SOURCES} ${TOOLS_RC_FILE})
else()
  add_executable(saunafs ${TOOLS_MAIN} ${TOOLS_SOURCES} ${ICON_RESOURCE_FILE} ${TOOLS_RC_FILE})
endif()
target_link_libraries(saunafs sfscommon)

install(TARGETS saunafs DESTINATION ${BIN_SUBDIR} COMPONENT client)
