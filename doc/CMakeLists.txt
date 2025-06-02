if(NOT A2X_BINARY)
  message(WARNING "Program 'a2x' not found, manpages won't be generated")
  return()
endif()

# These manpages should be generated
set(MANPAGES
    saunafs.7
    saunafs-admin.8
    saunafs-cgiserver.8
    saunafs-probe.8          # not a source
    saunafs-uraft.8
    saunafs-uraft-helper.8
    saunafs-uraft.cfg.5
    saunafs-appendchunks.1
    saunafs-checkfile.1
    saunafs-dirinfo.1
    saunafs-fileinfo.1
    saunafs-filerepair.1
    saunafs-geteattr.1
    saunafs-seteattr.1       # not a source
    saunafs-deleattr.1       # not a source
    saunafs-getgoal.1
    saunafs-setgoal.1        # not a source
    saunafs-rgetgoal.1       # not a source
    saunafs-rsetgoal.1       # not a source
    saunafs-gettrashtime.1
    saunafs-settrashtime.1   # not a source
    saunafs-rgettrashtime.1  # not a source
    saunafs-rsettrashtime.1  # not a source
    saunafs-makesnapshot.1
    saunafs-rremove.1
    sfsmount.1
    saunafs-repquota.1
    saunafs-setquota.1       # not a source
    saunafs.1
    sfsglobaliolimits.cfg.5
    sfsiolimits.cfg.5
    sfschunkserver.cfg.5
    sfsexports.cfg.5
    sfsgoals.cfg.5
    sfshdd.cfg.5
    sfsmaster.cfg.5
    sfsmetalogger.cfg.5
    sfsmount.cfg.5
    sfstopology.cfg.5
    sfsmetadump.8
    sfsrestoremaster.8
    sfs.7                     # not a source
    sfschunkserver.8
    saunafs-migrations.7
    sfsmaster.8
    sfsmetalogger.8
    sfsmetarestore.8
)

# Create custom directory to store the a2x logs
set(A2X_LOGS_DIR ${CMAKE_CURRENT_BINARY_DIR}/a2x)
execute_process(COMMAND mkdir -p ${A2X_LOGS_DIR})

foreach(MANPAGE ${MANPAGES})
  get_filename_component(MANPAGE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/${MANPAGE}.adoc ABSOLUTE)
  set(GENERATED_MANPAGE_PATH ${CMAKE_CURRENT_BINARY_DIR}/${MANPAGE})
  if(ENABLE_VERBOSE_ASCIIDOC)
    set(ASCIIDOC_VERBOSE --verbose --keep-artifacts)
  else()
    set(ASCIIDOC_VERBOSE)
  endif()
  if(EXISTS ${MANPAGE_SRC})
    set(MANPAGE_SRC_SYMLINK ${CMAKE_CURRENT_BINARY_DIR}/${MANPAGE}.txt)
    add_custom_command(OUTPUT ${GENERATED_MANPAGE_PATH}
        COMMAND ln -s -f ${MANPAGE_SRC} ${MANPAGE_SRC_SYMLINK}
        # Grepping out 'invalid escape sequence' warnings due to asciidoc bug
        COMMAND a2x ${ASCIIDOC_VERBOSE} -L -f manpage ${MANPAGE_SRC_SYMLINK} 2>&1
            | grep -v 'invalid escape sequence' | tee ${A2X_LOGS_DIR}/${MANPAGE}.log
        DEPENDS ${MANPAGE_SRC})
    set(GENERATED_MANPAGES ${GENERATED_MANPAGES} ${GENERATED_MANPAGE_PATH})
  endif()
  string(REGEX MATCH "[0-9]$" SECTION_NUMBER ${MANPAGE})
  install(FILES ${GENERATED_MANPAGE_PATH} DESTINATION ${MAN_SUBDIR}/man${SECTION_NUMBER})
endforeach()

add_custom_target(manpages ALL SOURCES ${GENERATED_MANPAGES})
