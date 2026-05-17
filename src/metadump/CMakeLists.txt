include_directories(${CMAKE_CURRENT_SOURCE_DIR})

aux_source_directory(. METADUMP_SOURCES)

add_executable(leil-metadump ${METADUMP_SOURCES})
target_link_libraries(leil-metadump)

install(TARGETS leil-metadump RUNTIME DESTINATION ${SBIN_SUBDIR})

add_custom_target(sfsmetadump-symlink ALL
  COMMAND ln -sf leil-metadump sfsmetadump
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS leil-metadump)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sfsmetadump DESTINATION ${SBIN_SUBDIR})
