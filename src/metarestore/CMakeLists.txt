include_directories(${CMAKE_CURRENT_SOURCE_DIR} ../master)
add_definitions(-DMETARESTORE)
add_definitions(-DAPPNAME=sfsmetarestore -DAPP_EXAMPLES_SUBDIR="${USR_SHARE_DOC}")

collect_sources(METARESTORE)

file(GLOB METARESTORE_MASTER_SOURCES ../master/filesystem*.cc)

if(DB_FOUND)
  file(GLOB METARESTORE_HSTRING_SOURCES ../master/hstring_*storage.cc)
else()
  file(GLOB METARESTORE_HSTRING_SOURCES ../master/hstring_memstorage.cc)
endif()

file(GLOB METARESTORE_BACKEND_SOURCES ../master/metadata_backend*.cc)

add_library(metarestore ${METARESTORE_SOURCES} ${METARESTORE_MASTER_SOURCES} ${METARESTORE_HSTRING_SOURCES}
  ${METARESTORE_BACKEND_SOURCES}
  ../master/acl_storage.cc ../master/chunks.cc ../master/quota_database.cc ../master/chunk_goal_counters.cc
  ../master/restore.cc ../master/locks.cc ../master/task_manager.cc ../master/snapshot_task.cc
  ../master/metadata_loader.cc ../master/setgoal_task.cc ../master/settrashtime_task.cc)

target_link_libraries(metarestore metrics sfscommon)
if(JUDY_LIBRARY)
  target_link_libraries(metarestore ${JUDY_LIBRARY})
endif()

create_unittest(metarestore ${METARESTORE_TESTS})

add_executable(sfsmetarestore ${METARESTORE_MAIN})
target_link_libraries(sfsmetarestore metarestore sfscommon ${PAM_LIBRARIES})
install(TARGETS sfsmetarestore RUNTIME DESTINATION ${SBIN_SUBDIR})
