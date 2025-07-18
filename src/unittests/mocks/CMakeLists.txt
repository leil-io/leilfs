include_directories(${GTEST_INCLUDE_DIRS})

collect_sources(UNITTESTMOCKS)
add_library(unittest-mocks ${UNITTESTMOCKS_SOURCES})
create_unittest(unittest-mocks ${UNITTESTMOCKS_TESTS})
link_unittest(unittest-mocks unittest-mocks sfscommon)
set_target_properties(unittest-mocks PROPERTIES UNITY_BUILD OFF)
