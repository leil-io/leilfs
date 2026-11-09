# Test-only plugin: it fakes chunks that have no data behind them, so it must
# never reach a production install. Built and installed only for test builds,
# where the chunkserver loads it from the installed plugins directory.
if(NOT BUILD_TESTS)
  return()
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
collect_sources(MOCK_DISK_PLUGIN)

set(plugin_name "mock-disk-plugin")

add_library("${plugin_name}" SHARED ${MOCK_DISK_PLUGIN_SOURCES})

set_target_properties("${plugin_name}" PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/chunkserver)

target_link_libraries("${plugin_name}" chunkserver-common_pic leil-common_pic
        ${Boost_LIBRARIES} ${ADDITIONAL_LIBS})

install(TARGETS "${plugin_name}"
        LIBRARY DESTINATION "${PLUGINS_PATH}/chunkserver"
        COMPONENT "${plugin_name}")
