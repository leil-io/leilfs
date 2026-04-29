configure_file(leil-uraft-helper.in leil-uraft-helper @ONLY)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

collect_sources(URAFT)
set(URAFT_SOURCES ${URAFT_SOURCES} ${CMAKE_SOURCE_DIR}/src/common/time_utils.cc)

add_library(uraft ${URAFT_SOURCES})
target_link_libraries(uraft ${Boost_LIBRARIES} ${RT_LIBRARY} pthread)

create_unittest(uraft ${URAFT_TESTS})
link_unittest(uraft uraft pthread leil-common)

add_executable(leil-uraft ${URAFT_MAIN} ${URAFT_SOURCES})
target_link_libraries(leil-uraft uraft)

install(TARGETS leil-uraft RUNTIME DESTINATION ${SBIN_SUBDIR})

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/leil-uraft-helper DESTINATION ${SBIN_SUBDIR})

add_custom_target(saunafs-uraft-symlink ALL
  COMMAND ln -sf leil-uraft saunafs-uraft
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS leil-uraft
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/saunafs-uraft DESTINATION ${SBIN_SUBDIR})

add_custom_target(saunafs-uraft-helper-symlink ALL
  COMMAND ln -sf leil-uraft-helper saunafs-uraft-helper
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS leil-uraft-helper
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/saunafs-uraft-helper DESTINATION ${SBIN_SUBDIR})
