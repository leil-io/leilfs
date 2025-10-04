if(NOT ASCIIDOCTOR_BINARY)
    message(WARNING "Program 'asciidoctor' not found, manpages won't be generated")
    return()
endif()

# These manpages should be generated
set(MANPAGES
    saunafs-admin.8
    saunafs-appendchunks.1
    saunafs-cgiserver.8
    saunafs-checkfile.1
    saunafs-deleattr.1       # not a source
    saunafs-dirinfo.1
    saunafs-fileinfo.1
    saunafs-filerepair.1
    saunafs-geteattr.1
    saunafs-getgoal.1
    saunafs-gettrashtime.1
    saunafs-makesnapshot.1
    saunafs-migrations.7
    saunafs-probe.8          # not a source
    saunafs-repquota.1
    saunafs-rgetgoal.1       # not a source
    saunafs-rgettrashtime.1  # not a source
    saunafs-rremove.1
    saunafs-rsetgoal.1       # not a source
    saunafs-rsettrashtime.1  # not a source
    saunafs-seteattr.1       # not a source
    saunafs-setgoal.1        # not a source
    saunafs-setquota.1       # not a source
    saunafs-settrashtime.1   # not a source
    saunafs-uraft-helper.8
    saunafs-uraft.8
    saunafs-uraft.cfg.5
    saunafs.1
    saunafs.7
    sfs.7                    # not a source
    sfschunkserver.8
    sfschunkserver.cfg.5
    sfsexports.cfg.5
    sfsglobaliolimits.cfg.5
    sfsgoals.cfg.5
    sfshdd.cfg.5
    sfsiolimits.cfg.5
    sfsmaster.8
    sfsmaster.cfg.5
    sfsmetadump.8
    sfsmetalogger.8
    sfsmetalogger.cfg.5
    sfsmetarestore.8
    sfsmount.1
    sfsmount.cfg.5
    sfsrestoremaster.8
    sfstopology.cfg.5
)

# Create custom directory to store the a2x logs
set(A2X_LOGS_DIR ${CMAKE_CURRENT_BINARY_DIR}/a2x)
execute_process(COMMAND mkdir -p ${A2X_LOGS_DIR})

foreach(MANPAGE ${MANPAGES})
    get_filename_component(MANPAGE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/${MANPAGE}.adoc ABSOLUTE)
    set(GENERATED_MANPAGE_PATH ${CMAKE_CURRENT_BINARY_DIR}/${MANPAGE})
    if(ENABLE_VERBOSE_ASCIIDOCTOR)
        set(ASCIIDOCTOR_VERBOSE_ARGS --trace --verbose)
    else()
        set(ASCIIDOCTOR_VERBOSE_ARGS)
    endif()
    if(EXISTS ${MANPAGE_SRC})
        set(MANPAGE_SRC_SYMLINK ${CMAKE_CURRENT_BINARY_DIR}/${MANPAGE}.txt)
        add_custom_command(
                OUTPUT ${GENERATED_MANPAGE_PATH}
                COMMAND ${CMAKE_COMMAND} -E create_symlink ${MANPAGE_SRC} ${MANPAGE_SRC_SYMLINK}
                COMMAND ${ASCIIDOCTOR_BINARY} ${ASCIIDOCTOR_VERBOSE_ARGS} -b manpage
                    -o ${GENERATED_MANPAGE_PATH} ${MANPAGE_SRC_SYMLINK}
                DEPENDS ${MANPAGE_SRC}
                COMMENT "Generating manpage ${MANPAGE}"
                VERBATIM
        )
        list(APPEND GENERATED_MANPAGES ${GENERATED_MANPAGE_PATH})
    endif()

    string(REGEX MATCH "[0-9]$" SECTION_NUMBER ${MANPAGE})
    install(FILES ${GENERATED_MANPAGE_PATH} DESTINATION ${MAN_SUBDIR}/man${SECTION_NUMBER})
endforeach()

add_custom_target(manpages ALL SOURCES ${GENERATED_MANPAGES})
