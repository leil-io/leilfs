include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(CHUNKSERVER_PLUGINS)
shared_add_library(chunkserver-common ${CHUNKSERVER_PLUGINS_SOURCES})
target_link_libraries(chunkserver-common safsprotocol sfscommon
        ${Boost_LIBRARIES} ${ADDITIONAL_LIBS})
list(REMOVE_ITEM CHUNKSERVER_PLUGINS_TESTS
        ${CMAKE_CURRENT_SOURCE_DIR}/cmr_disk_unittest.cc)
create_unittest(chunkserver-common ${CHUNKSERVER_PLUGINS_TESTS})
link_unittest(chunkserver-common gmock gtest chunkserver-common sfscommon)

add_executable(chunkserver-common-mocked-time-unittest
        ${CMAKE_CURRENT_SOURCE_DIR}/cmr_disk_unittest.cc)
target_link_options(chunkserver-common-mocked-time-unittest PRIVATE "-Wl,--wrap=time")
target_link_libraries(chunkserver-common-mocked-time-unittest gmock gtest
        chunkserver-common gtest_main sfscommon ${Boost_LIBRARIES}
        ${ADDITIONAL_LIBS})
add_test(NAME CmrDiskTest COMMAND chunkserver-common-mocked-time-unittest)
