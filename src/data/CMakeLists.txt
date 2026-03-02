configure_file(sfsmaster.cfg.in sfsmaster.cfg)
configure_file(sfschunkserver.cfg.in sfschunkserver.cfg)
configure_file(sfsmetalogger.cfg.in sfsmetalogger.cfg)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sfsmds.cfg.in)
  configure_file(sfsmds.cfg.in sfsmds.cfg)
endif()
configure_file(postinst.in postinst)
set(POSTINST_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/postinst PARENT_SCOPE)

install(FILES metadata.sfs                                   DESTINATION ${DATA_SUBDIR} RENAME metadata.sfs.empty)
install(FILES sfsexports.cfg                                 DESTINATION ${SFSMASTER_EXAMPLES_SUBDIR})
install(FILES sfsgoals.cfg                                   DESTINATION ${SFSMASTER_EXAMPLES_SUBDIR})
install(FILES sfshdd.cfg                                     DESTINATION ${CHUNKSERVER_EXAMPLES_SUBDIR})
install(FILES sfsmount.cfg                                   DESTINATION ${CLIENT_EXAMPLES_SUBDIR} COMPONENT client)
install(FILES sfsiolimits.cfg                                DESTINATION ${CLIENT_EXAMPLES_SUBDIR} COMPONENT client)
install(FILES sfstls.cfg                                     DESTINATION ${CLIENT_EXAMPLES_SUBDIR} COMPONENT client)
install(FILES sfstopology.cfg                                DESTINATION ${SFSMASTER_EXAMPLES_SUBDIR})
install(FILES sfsglobaliolimits.cfg                             DESTINATION ${SFSMASTER_EXAMPLES_SUBDIR})
install(FILES saunafs-uraft.cfg                             DESTINATION ${URAFT_EXAMPLES_SUBDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sfsmaster.cfg      DESTINATION ${SFSMASTER_EXAMPLES_SUBDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sfschunkserver.cfg DESTINATION ${CHUNKSERVER_EXAMPLES_SUBDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sfsmetalogger.cfg  DESTINATION ${METALOGGER_EXAMPLES_SUBDIR})
if(EXISTS ${CMAKE_CURRENT_BINARY_DIR}/sfsmds.cfg)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sfsmds.cfg       DESTINATION ${SFSMDS_EXAMPLES_SUBDIR})
endif()

install(FILES saunafs.completion DESTINATION etc/bash_completion.d RENAME saunafs COMPONENT client)

if(BUILD_TESTS)
  # Create a mock include dir.
  # We do this because we want to preserve the proper include directives in the example code
  # and still build this example program in the build step before the needed headers are installed.
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/saunafs)
  add_custom_command(
    OUTPUT
      ${CMAKE_CURRENT_BINARY_DIR}/include/saunafs/saunafs_c_api.h
      ${CMAKE_CURRENT_BINARY_DIR}/include/saunafs/saunafs_error_codes.h
    COMMAND ${CMAKE_COMMAND} -E create_symlink
      ${CMAKE_SOURCE_DIR}/src/mount/client/saunafs_c_api.h
      ${CMAKE_CURRENT_BINARY_DIR}/include/saunafs/saunafs_c_api.h
    COMMAND ${CMAKE_COMMAND} -E create_symlink
      ${CMAKE_SOURCE_DIR}/src/errors/saunafs_error_codes.h
      ${CMAKE_CURRENT_BINARY_DIR}/include/saunafs/saunafs_error_codes.h
  )
  add_executable(c-client-example
    libsaunafs-client-example.c
    ${CMAKE_CURRENT_BINARY_DIR}/include/saunafs/saunafs_c_api.h
    ${CMAKE_CURRENT_BINARY_DIR}/include/saunafs/saunafs_error_codes.h)
  target_include_directories(c-client-example PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/include)
  target_link_libraries(c-client-example saunafs-client stdc++ m)
  install(TARGETS c-client-example RUNTIME DESTINATION ${BIN_SUBDIR})
endif()
