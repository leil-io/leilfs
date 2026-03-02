include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_subdirectory(sfs_fuselib)

collect_sources(MOUNT_FUSE)

add_executable(sfsmount ${MOUNT_FUSE_MAIN} ${MOUNT_FUSE_SOURCES})
target_link_libraries(sfsmount mount sfscommon sfs_fuselib ${FUSE3_LIBRARY})
target_include_directories(sfsmount PRIVATE ${FUSE3_INCLUDE_DIR})
target_compile_definitions(sfsmount PRIVATE "-DAPPNAME=sfsmount" "-DCFGNAME=sfsmount" -DAPP_EXAMPLES_DIRECTORY="${CLIENT_EXAMPLES_SUBDIR}" ${FUSE3_CFLAGS_OTHER} "-DFUSE_USE_VERSION=30")
install(TARGETS sfsmount RUNTIME DESTINATION ${BIN_SUBDIR} COMPONENT client)
