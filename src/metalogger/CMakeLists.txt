include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_definitions(-DMETALOGGER)
add_definitions(-DAPPNAME=leil-metalogger -DAPPNAME_LEGACY=sfsmetalogger -DAPP_EXAMPLES_SUBDIR="${METALOGGER_EXAMPLES_SUBDIR}")

collect_sources(METALOGGER)

add_library(metalogger ${METALOGGER_SOURCES}
    ../master/changelog.cc
    ../master/masterconn.cc
    ../master/metadata_backend_file.cc
)
target_link_libraries(metalogger leil-common)

create_unittest(metalogger ${METALOGGER_TESTS})

add_executable(leil-metalogger ${MAIN_SRC})
target_link_libraries(leil-metalogger metalogger ${PAM_LIBRARIES})

if(SYSTEMD_FOUND)
  target_link_libraries(leil-metalogger ${SYSTEMD_LIBRARIES})
endif()

install(TARGETS leil-metalogger RUNTIME DESTINATION ${SBIN_SUBDIR})
add_custom_target(sfsmetalogger-symlink ALL
  COMMAND ln -sf leil-metalogger sfsmetalogger
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS leil-metalogger)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sfsmetalogger DESTINATION ${SBIN_SUBDIR})
