include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(TOOLS)

set(TOOLS_RC_FILE "")
if(MINGW)
  include(WindowsVersion)
  sfs_configure_version_rc(
    "${CMAKE_CURRENT_BINARY_DIR}/leil-version.rc"
    "LeilFS Tools"
    "Tools for LeilFS"
    "leil"
    "leil.exe"
  )
  set(TOOLS_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/leil-version.rc")
endif()

if(ICON_RESOURCE_FILE STREQUAL "")
  add_executable(leil ${TOOLS_MAIN} ${TOOLS_SOURCES} ${TOOLS_RC_FILE})
else()
  add_executable(leil ${TOOLS_MAIN} ${TOOLS_SOURCES} ${ICON_RESOURCE_FILE} ${TOOLS_RC_FILE})
endif()
target_link_libraries(leil leil-common)

install(TARGETS leil DESTINATION ${BIN_SUBDIR} COMPONENT client)

if(MINGW)
  add_custom_command(
    TARGET leil POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:leil> ${CMAKE_CURRENT_BINARY_DIR}/saunafs.exe
  )
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/saunafs.exe DESTINATION ${BIN_SUBDIR} COMPONENT client)
else()
  add_custom_target(saunafs-symlink ALL
    COMMAND ln -sf leil saunafs
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS leil)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/saunafs DESTINATION ${BIN_SUBDIR} COMPONENT client)
endif()
