include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_definitions(-DMETALOGGER)
add_definitions(-DAPPNAME=sfsmetalogger -DAPP_EXAMPLES_SUBDIR="${METALOGGER_EXAMPLES_SUBDIR}")

collect_sources(METALOGGER)

add_library(metalogger ${METALOGGER_SOURCES} ../master/changelog.cc ../master/masterconn.cc
	../master/metadata_backend_file.cc ../master/changelog_db_fdb.cc
)
target_link_libraries(metalogger sfscommon)

if(ENABLE_FOUNDATIONDB)
  target_link_libraries(metalogger fdb)
endif()

create_unittest(metalogger ${METALOGGER_TESTS})

add_executable(sfsmetalogger ${MAIN_SRC})
target_link_libraries(sfsmetalogger metalogger ${PAM_LIBRARIES})
if(SYSTEMD_FOUND)
  target_link_libraries(sfsmetalogger ${SYSTEMD_LIBRARIES})
endif()
install(TARGETS sfsmetalogger RUNTIME DESTINATION ${SBIN_SUBDIR})
