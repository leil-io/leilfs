include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_definitions(-DAPPNAME=leil-master -DAPPNAME_LEGACY=sfsmaster -DAPP_EXAMPLES_SUBDIR="${SFSMASTER_EXAMPLES_SUBDIR}")

collect_sources(MASTER)

if(NOT DB_FOUND)
  list(REMOVE_ITEM MASTER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/hstring_bdbstorage.cc")
endif()

if(NOT ENABLE_FOUNDATIONDB)
  list(REMOVE_ITEM MASTER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kv_connector_fdb.cc")
endif()

option(DISABLE_JUDY_FOR_DEFECTIVENODESMAP       "Disable the use of judy for the defective inodes map."                OFF)
option(DISABLE_JUDY_FOR_TRASHPATHCONTAINER      "Disable the use of judy for the trash path container."                OFF)
option(DISABLE_JUDY_FOR_RESERVEDPATHCONTAINER   "Disable the use of judy for the reserved path container."             OFF)

if(DISABLE_JUDY_FOR_DEFECTIVENODESMAP)
  message(STATUS "Judy for defective inodes map disabled")
  add_definitions(-DDISABLE_JUDY_FOR_DEFECTIVENODESMAP)
else()
  message(STATUS "Judy for defective inodes map enabled (if available)")
endif()
if(DISABLE_JUDY_FOR_TRASHPATHCONTAINER)
  message(STATUS "Judy for trash path container disabled")
  add_definitions(-DDISABLE_JUDY_FOR_TRASHPATHCONTAINER)
else()
  message(STATUS "Judy for trash path container enabled (if available)")
endif()
if(DISABLE_JUDY_FOR_RESERVEDPATHCONTAINER)
  message(STATUS "Judy for reserved path container disabled")
  add_definitions(-DDISABLE_JUDY_FOR_RESERVEDPATHCONTAINER)
else()
  message(STATUS "Judy for reserved path container enabled (if available)")
endif()

add_library(master ${MASTER_SOURCES})
target_link_libraries(master leil-common metrics ${ADDITIONAL_LIBS})
if(ENABLE_FOUNDATIONDB)
  target_link_libraries(master fdb)
endif()
if(JUDY_LIBRARY)
  target_link_libraries(master ${JUDY_LIBRARY})
endif()
if(DB_FOUND)
  target_link_libraries(master ${DB_LIBRARY})
endif()
create_unittest(master ${MASTER_TESTS})
link_unittest(master master leil-common)

add_executable(leil-master ${MAIN_SRC})
target_link_libraries(leil-master master metrics ${PAM_LIBRARIES})
if(SYSTEMD_FOUND)
  target_link_libraries(leil-master ${SYSTEMD_LIBRARIES})
endif()
install(TARGETS leil-master RUNTIME DESTINATION ${SBIN_SUBDIR})
add_custom_target(sfsmaster-symlink ALL
  COMMAND ln -sf leil-master sfsmaster
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS leil-master)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sfsmaster DESTINATION ${SBIN_SUBDIR})

configure_file(sfsrestoremaster.in leil-restoremaster @ONLY)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/leil-restoremaster DESTINATION ${SBIN_SUBDIR})

add_custom_target(sfsrestoremaster-symlink ALL
  COMMAND ln -sf leil-restoremaster sfsrestoremaster
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS leil-restoremaster)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sfsrestoremaster DESTINATION ${SBIN_SUBDIR})
