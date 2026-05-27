include_directories(${CMAKE_CURRENT_SOURCE_DIR} ../master)
add_definitions(-DMETARESTORE)
add_definitions(-DAPPNAME=leil-metarestore -DAPPNAME_LEGACY=sfsmetarestore -DAPP_EXAMPLES_SUBDIR="${USR_SHARE_DOC}")

collect_sources(METARESTORE)

file(GLOB METARESTORE_MASTER_SOURCES ../master/filesystem*.cc)
# Exclude possible matching unit tests files in the previous glob
list(FILTER METARESTORE_MASTER_SOURCES EXCLUDE REGEX ".*unittest.*")

if(DB_FOUND)
  file(GLOB METARESTORE_HSTRING_SOURCES ../master/hstring_*storage.cc)
else()
  file(GLOB METARESTORE_HSTRING_SOURCES ../master/hstring_memstorage.cc)
endif()

file(GLOB METARESTORE_BACKEND_SOURCES ../master/metadata_backend*.cc)
# Exclude possible matching unit tests files in the previous glob
list(FILTER METARESTORE_BACKEND_SOURCES EXCLUDE REGEX ".*unittest.*")

add_library(metarestore ${METARESTORE_SOURCES} ${METARESTORE_MASTER_SOURCES} ${METARESTORE_HSTRING_SOURCES}
  ${METARESTORE_BACKEND_SOURCES}
  ../master/acl_storage.cc ../master/chunks.cc ../master/chunk_operations_base.cc
  ../common/quota_database.cc ../master/chunk_goal_counters.cc
  ../master/restore.cc ../master/locks.cc ../master/task_manager.cc ../master/snapshot_task.cc
  ../master/metadata_loader.cc ../master/setgoal_task.cc ../master/settrashtime_task.cc
  ../master/changelog.cc)

target_link_libraries(metarestore metrics leil-common)
if(JUDY_LIBRARY)
  target_link_libraries(metarestore ${JUDY_LIBRARY})
endif()

create_unittest(metarestore ${METARESTORE_TESTS})

add_executable(leil-metarestore ${METARESTORE_MAIN})
target_link_libraries(leil-metarestore metarestore leil-common ${PAM_LIBRARIES})
install(TARGETS leil-metarestore RUNTIME DESTINATION ${SBIN_SUBDIR})

add_custom_target(sfsmetarestore-symlink ALL
  COMMAND ln -sf leil-metarestore sfsmetarestore
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS leil-metarestore)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sfsmetarestore DESTINATION ${SBIN_SUBDIR})
