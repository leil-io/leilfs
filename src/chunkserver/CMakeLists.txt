include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_definitions(-DSAUNAFS_MAX_FILES=10000)
add_definitions(-DAPPNAME=leil-chunkserver -DAPPNAME_LEGACY=sfschunkserver -DAPP_EXAMPLES_SUBDIR="${CHUNKSERVER_EXAMPLES_SUBDIR}")

add_subdirectory(chunkserver-common)
add_subdirectory(plugins)

collect_sources(CHUNKSERVER)
add_library(chunkserver ${CHUNKSERVER_SOURCES})
target_link_libraries(chunkserver chunkserver-common safsprotocol leil-common
    ${ADDITIONAL_LIBS})
create_unittest(chunkserver ${CHUNKSERVER_TESTS})
link_unittest(chunkserver chunkserver leil-common)

add_executable(leil-chunkserver ${MAIN_SRC})
target_link_libraries(leil-chunkserver chunkserver ${PAM_LIBRARIES})

if(SYSTEMD_FOUND)
  target_link_libraries(leil-chunkserver ${SYSTEMD_LIBRARIES})
endif()

install(TARGETS leil-chunkserver RUNTIME DESTINATION ${SBIN_SUBDIR})

add_custom_target(sfschunkserver-symlink ALL
  COMMAND ln -sf leil-chunkserver sfschunkserver
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS leil-chunkserver)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sfschunkserver DESTINATION ${SBIN_SUBDIR})
