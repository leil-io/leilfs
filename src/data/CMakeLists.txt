configure_file(leil-master.cfg.in leil-master.cfg)
configure_file(leil-chunkserver.cfg.in leil-chunkserver.cfg)
configure_file(leil-metalogger.cfg.in leil-metalogger.cfg)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/leil-mds.cfg.in)
  configure_file(leil-mds.cfg.in leil-mds.cfg)
endif()
configure_file(postinst.in postinst)
set(POSTINST_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/postinst PARENT_SCOPE)

function(install_relative_symlink target link_name destination)
  install(CODE "
    file(CREATE_LINK
      \"${target}\"
      \"\$ENV{DESTDIR}${destination}/${link_name}\"
      SYMBOLIC)
  " ${ARGN})
endfunction()

install(FILES metadata.sfs                                   DESTINATION ${DATA_SUBDIR} RENAME metadata.sfs.empty)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/leil-exports.cfg      DESTINATION ${SFSMASTER_EXAMPLES_SUBDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/leil-goals.cfg        DESTINATION ${SFSMASTER_EXAMPLES_SUBDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/leil-hdd.cfg          DESTINATION ${CHUNKSERVER_EXAMPLES_SUBDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/leil-mount.cfg        DESTINATION ${CLIENT_EXAMPLES_SUBDIR} COMPONENT client)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/leil-iolimits.cfg     DESTINATION ${CLIENT_EXAMPLES_SUBDIR} COMPONENT client)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/leil-tls.cfg          DESTINATION ${CLIENT_EXAMPLES_SUBDIR} COMPONENT client)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/leil-topology.cfg     DESTINATION ${SFSMASTER_EXAMPLES_SUBDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/leil-globaliolimits.cfg DESTINATION ${SFSMASTER_EXAMPLES_SUBDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/leil-uraft.cfg        DESTINATION ${URAFT_EXAMPLES_SUBDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/leil-master.cfg      DESTINATION ${SFSMASTER_EXAMPLES_SUBDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/leil-chunkserver.cfg DESTINATION ${CHUNKSERVER_EXAMPLES_SUBDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/leil-metalogger.cfg  DESTINATION ${METALOGGER_EXAMPLES_SUBDIR})
if(EXISTS ${CMAKE_CURRENT_BINARY_DIR}/leil-mds.cfg)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/leil-mds.cfg       DESTINATION ${SFSMDS_EXAMPLES_SUBDIR})
  install_relative_symlink(leil-mds.cfg sfsmds.cfg ${SFSMDS_EXAMPLES_SUBDIR})
endif()

install_relative_symlink(leil-exports.cfg sfsexports.cfg ${SFSMASTER_EXAMPLES_SUBDIR})
install_relative_symlink(leil-goals.cfg sfsgoals.cfg ${SFSMASTER_EXAMPLES_SUBDIR})
install_relative_symlink(leil-topology.cfg sfstopology.cfg ${SFSMASTER_EXAMPLES_SUBDIR})
install_relative_symlink(leil-globaliolimits.cfg sfsglobaliolimits.cfg ${SFSMASTER_EXAMPLES_SUBDIR})
install_relative_symlink(leil-master.cfg sfsmaster.cfg ${SFSMASTER_EXAMPLES_SUBDIR})
install_relative_symlink(leil-hdd.cfg sfshdd.cfg ${CHUNKSERVER_EXAMPLES_SUBDIR})
install_relative_symlink(leil-chunkserver.cfg sfschunkserver.cfg ${CHUNKSERVER_EXAMPLES_SUBDIR})
install_relative_symlink(leil-metalogger.cfg sfsmetalogger.cfg ${METALOGGER_EXAMPLES_SUBDIR})
install_relative_symlink(leil-mount.cfg sfsmount.cfg ${CLIENT_EXAMPLES_SUBDIR} COMPONENT client)
install_relative_symlink(leil-iolimits.cfg sfsiolimits.cfg ${CLIENT_EXAMPLES_SUBDIR} COMPONENT client)
install_relative_symlink(leil-tls.cfg sfstls.cfg ${CLIENT_EXAMPLES_SUBDIR} COMPONENT client)
install_relative_symlink(leil-uraft.cfg saunafs-uraft.cfg ${URAFT_EXAMPLES_SUBDIR})

install(FILES leil.completion DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/bash_completion.d RENAME leil COMPONENT client)
install_relative_symlink(leil saunafs ${CMAKE_INSTALL_FULL_SYSCONFDIR}/bash_completion.d COMPONENT client)

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
