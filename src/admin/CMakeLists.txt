collect_sources(LEIL_ADMIN)

add_library(leil-admin-lib ${LEIL_ADMIN_SOURCES})
target_link_libraries(leil-admin-lib leil-common)
create_unittest(leil-admin-lib ${LEIL_ADMIN_TESTS})
link_unittest(leil-admin-lib leil-common)

set(ADMIN_RC_FILE "")
if(MINGW)
  include(WindowsVersion)
  sfs_configure_version_rc(
    "${CMAKE_CURRENT_BINARY_DIR}/leil-admin-version.rc"
    "Leil Admin"
    "Administrative Tool for LeilFS"
    "leil-admin"
    "leil-admin.exe"
  )
  set(ADMIN_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/leil-admin-version.rc")
endif()

if(ICON_RESOURCE_FILE STREQUAL "")
  add_executable(leil-admin ${LEIL_ADMIN_MAIN} ${ADMIN_RC_FILE})
else()
  add_executable(leil-admin ${LEIL_ADMIN_MAIN} ${ICON_RESOURCE_FILE} ${ADMIN_RC_FILE})
endif()
target_link_libraries(leil-admin leil-admin-lib)
install(TARGETS leil-admin RUNTIME DESTINATION ${BIN_SUBDIR})

if(MINGW)
  add_custom_command(
    TARGET leil-admin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:leil-admin> ${CMAKE_CURRENT_BINARY_DIR}/saunafs-admin.exe
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:leil-admin> ${CMAKE_CURRENT_BINARY_DIR}/saunafs-probe.exe
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:leil-admin> ${CMAKE_CURRENT_BINARY_DIR}/leil-probe.exe
  )
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/saunafs-admin.exe
    ${CMAKE_CURRENT_BINARY_DIR}/saunafs-probe.exe
    ${CMAKE_CURRENT_BINARY_DIR}/leil-probe.exe
    DESTINATION ${BIN_SUBDIR}
  )
else()
  add_custom_target(saunafs-admin-symlink ALL
    COMMAND ln -sf leil-admin saunafs-admin
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS leil-admin
  )
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/saunafs-admin DESTINATION ${BIN_SUBDIR})

  add_custom_target(saunafs-probe-symlink ALL
    COMMAND ln -sf leil-admin saunafs-probe
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS leil-admin)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/saunafs-probe DESTINATION ${BIN_SUBDIR})

  add_custom_target(leil-probe-symlink ALL
    COMMAND ln -sf leil-admin leil-probe
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS leil-admin)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/leil-probe DESTINATION ${BIN_SUBDIR})
endif()
