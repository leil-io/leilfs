include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(TOOLS)

if(ICON_RESOURCE_FILE STREQUAL "")
  add_executable(saunafs ${TOOLS_MAIN} ${TOOLS_SOURCES})
else()
  add_executable(saunafs ${TOOLS_MAIN} ${TOOLS_SOURCES} ${ICON_RESOURCE_FILE})
endif()
target_link_libraries(saunafs sfscommon)
install(TARGETS saunafs DESTINATION ${BIN_SUBDIR})

# Test utility for changelog integration tests
add_executable(changelog_test_util changelog_test_util.cc)
target_link_libraries(changelog_test_util sfscommon master) # master for changelog.h/cc, sfscommon for utils
# No install command for this utility
