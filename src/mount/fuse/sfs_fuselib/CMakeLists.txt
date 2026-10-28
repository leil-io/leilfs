include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(SFS_FUSELIB)

shared_add_library(sfs_fuselib ${SFS_FUSELIB_SOURCES})
shared_target_link_libraries(sfs_fuselib slogger)
create_unittest(sfs_fuselib ${SAUNAFS_FUSE_LIB_TESTS})
link_unittest(sfs_fuselib sfs_fuselib leil-common)
