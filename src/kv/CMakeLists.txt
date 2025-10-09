include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(KV)

add_library(kv ${KV_SOURCES})
target_link_libraries(kv slogger)

create_unittest(kv ${KV_TESTS})
link_unittest(kv kv sfscommon)
