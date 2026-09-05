include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(CHUNKSERVER_PLUGINS)
shared_add_library(chunkserver-common ${CHUNKSERVER_PLUGINS_SOURCES})
shared_target_link_libraries(chunkserver-common
        safsprotocol
        leil-common
        ${Boost_LIBRARIES}
        ${ADDITIONAL_LIBS}
        ${ZSTD_LIBRARIES})

shared_target_link_libraries(chunkserver-common STATIC sfserr SHARED sfserr_pic)

create_unittest(chunkserver-common ${CHUNKSERVER_PLUGINS_TESTS})
link_unittest(chunkserver-common
        GTest::gtest
        GTest::gmock
        chunkserver-common
        leil-common)
