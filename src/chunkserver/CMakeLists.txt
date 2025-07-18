include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_definitions(-DSAUNAFS_MAX_FILES=10000)
add_definitions(-DAPPNAME=sfschunkserver -DAPP_EXAMPLES_SUBDIR="${CHUNKSERVER_EXAMPLES_SUBDIR}")

add_subdirectory(chunkserver-common)
add_subdirectory(plugins)

collect_sources(CHUNKSERVER)
add_library(chunkserver ${CHUNKSERVER_SOURCES})
target_link_libraries(chunkserver chunkserver-common safsprotocol sfscommon
    ${ADDITIONAL_LIBS})
set_target_properties(chunkserver PROPERTIES UNITY_BUILD OFF)
create_unittest(chunkserver ${CHUNKSERVER_TESTS})
link_unittest(chunkserver chunkserver sfscommon)

add_executable(sfschunkserver ${MAIN_SRC})
target_link_libraries(sfschunkserver chunkserver ${PAM_LIBRARIES})
set_target_properties(sfschunkserver PROPERTIES UNITY_BUILD OFF)

if(SYSTEMD_FOUND)
  target_link_libraries(sfschunkserver ${SYSTEMD_LIBRARIES})
endif()

install(TARGETS sfschunkserver RUNTIME DESTINATION ${SBIN_SUBDIR})
