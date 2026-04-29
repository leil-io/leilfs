include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(FDB)

add_library(fdb ${FDB_SOURCES})
target_link_libraries(fdb slogger kv fdb_c)

create_unittest(fdb ${FDB_TESTS})
link_unittest(fdb fdb leil-common)
