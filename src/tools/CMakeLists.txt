include_directories(${CMAKE_CURRENT_SOURCE_DIR})
set(SFSTOOL_LINKS
  sfsgetgoal
  sfssetgoal
  sfsgettrashtime
  sfssettrashtime
  sfscheckfile
  sfsfileinfo
  sfsappendchunks
  sfsdirinfo
  sfsfilerepair
  sfsmakesnapshot
  sfsgeteattr
  sfsseteattr
  sfsdeleattr
  sfsrgetgoal
  sfsrsetgoal
  sfsrgettrashtime
  sfsrsettrashtime
  sfssetquota
  sfsrepquota)

collect_sources(TOOLS)

add_executable(saunafs ${TOOLS_MAIN} ${TOOLS_SOURCES})
target_link_libraries(saunafs sfscommon)
install(TARGETS saunafs DESTINATION ${BIN_SUBDIR})

install(FILES sfstools.sh DESTINATION ${BIN_SUBDIR} PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
                    GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ)

foreach(LINK ${SFSTOOL_LINKS})
  if(NOT WIN32)	
    add_custom_target(${LINK} ALL
      COMMAND ln -sf sfstools.sh ${LINK}
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  endif()
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LINK} DESTINATION ${BIN_SUBDIR})
endforeach()
