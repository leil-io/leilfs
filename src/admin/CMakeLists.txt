collect_sources(SAUNAFS_ADMIN)

add_library(saunafs-admin-lib ${SAUNAFS_ADMIN_SOURCES})
target_link_libraries(saunafs-admin-lib sfscommon)
create_unittest(saunafs-admin-lib ${SAUNAFS_ADMIN_TESTS})
link_unittest(saunafs-admin-lib sfscommon)

set(ADMIN_RC_FILE "")
if(MINGW)
  include(WindowsVersion)
  sfs_configure_version_rc(
    "${CMAKE_CURRENT_BINARY_DIR}/saunafs-admin-version.rc"
    "SaunaFS Admin"
    "Administrative Tool for SaunaFS"
    "saunafs-admin"
    "saunafs-admin.exe"
  )
  set(ADMIN_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/saunafs-admin-version.rc")
endif()

if(ICON_RESOURCE_FILE STREQUAL "")
  add_executable(saunafs-admin ${SAUNAFS_ADMIN_MAIN} ${ADMIN_RC_FILE})
else()
  add_executable(saunafs-admin ${SAUNAFS_ADMIN_MAIN} ${ICON_RESOURCE_FILE} ${ADMIN_RC_FILE})
endif()
target_link_libraries(saunafs-admin saunafs-admin-lib)
install(TARGETS saunafs-admin RUNTIME DESTINATION ${BIN_SUBDIR})

if(NOT MINGW)
  add_custom_target(saunafs-probe ALL
    COMMAND ln -sf saunafs-admin saunafs-probe
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS saunafs-admin)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/saunafs-probe DESTINATION ${BIN_SUBDIR})
endif()
