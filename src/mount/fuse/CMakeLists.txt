include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_subdirectory(sfs_fuselib)

collect_sources(MOUNT_FUSE)

add_executable(leil-mount ${MOUNT_FUSE_MAIN} ${MOUNT_FUSE_SOURCES})
target_link_libraries(leil-mount mount leil-common sfs_fuselib ${FUSE3_LIBRARY})
target_include_directories(leil-mount PRIVATE ${FUSE3_INCLUDE_DIR})
target_compile_definitions(leil-mount PRIVATE "-DAPPNAME=leil-mount" "-DAPPNAME_LEGACY=sfsmount" "-DCFGNAME=leil-mount" "-DCFGNAME_LEGACY=sfsmount" -DAPP_EXAMPLES_DIRECTORY="${CLIENT_EXAMPLES_SUBDIR}" ${FUSE3_CFLAGS_OTHER} "-DFUSE_USE_VERSION=30")
install(TARGETS leil-mount RUNTIME DESTINATION ${BIN_SUBDIR} COMPONENT client)

add_custom_target(sfsmount-symlink ALL
  COMMAND ln -sf leil-mount sfsmount
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS leil-mount)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sfsmount DESTINATION ${BIN_SUBDIR} COMPONENT client)

