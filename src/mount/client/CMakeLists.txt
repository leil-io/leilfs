collect_sources(CLIENT)

shared_add_library(saunafs-client client.cc saunafs_c_api.cc client_error_code.cc)
shared_target_link_libraries(saunafs-client mount)

shared_add_library(saunafs-client-cpp client.cc client_error_code.cc)
shared_target_link_libraries(saunafs-client-cpp mount)

shared_target_link_libraries(saunafs-client ${CMAKE_DL_LIBS})

add_library(saunafs-client_shared SHARED client.cc saunafs_c_api.cc client_error_code.cc)
set_target_properties(saunafs-client_shared PROPERTIES OUTPUT_NAME "saunafs-client")
target_link_libraries(saunafs-client_shared ${CMAKE_DL_LIBS} mount_pic)

add_library(saunafsmount_shared SHARED ${MOUNT_SOURCES} sauna_client_c_linkage.cc)
target_link_libraries(saunafsmount_shared mount_pic ${ADDITIONAL_LIBS})

install(TARGETS saunafs-client DESTINATION ${LIB_SUBDIR} COMPONENT client)
install(TARGETS saunafs-client_pic DESTINATION ${LIB_SUBDIR} COMPONENT client)
install(TARGETS saunafs-client-cpp DESTINATION ${LIB_SUBDIR} COMPONENT client)
install(TARGETS saunafs-client-cpp_pic DESTINATION ${LIB_SUBDIR} COMPONENT client)
install(TARGETS saunafsmount_shared DESTINATION ${LIB_SUBDIR} COMPONENT client)
install(TARGETS saunafs-client_shared DESTINATION ${LIB_SUBDIR} COMPONENT client)

install(FILES saunafs_c_api.h DESTINATION ${INCL_SUBDIR} COMPONENT client)
install(FILES ../../errors/saunafs_error_codes.h DESTINATION ${INCL_SUBDIR} COMPONENT client)
