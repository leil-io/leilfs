include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(KV)

add_library(kv INTERFACE)

create_unittest(kv ${KV_TESTS})
link_unittest(kv kv leil-common)
