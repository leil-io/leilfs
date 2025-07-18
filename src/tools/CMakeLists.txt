include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(TOOLS)

if(ICON_RESOURCE_FILE STREQUAL "")
  add_executable(saunafs ${TOOLS_MAIN} ${TOOLS_SOURCES})
else()
  add_executable(saunafs ${TOOLS_MAIN} ${TOOLS_SOURCES} ${ICON_RESOURCE_FILE})
endif()
target_link_libraries(saunafs sfscommon)
set_target_properties(saunafs PROPERTIES UNITY_BUILD OFF)
install(TARGETS saunafs DESTINATION ${BIN_SUBDIR})
