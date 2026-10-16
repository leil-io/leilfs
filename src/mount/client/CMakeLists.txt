collect_sources(CLIENT)

shared_add_library(leil-client client.cc leil_c_api.cc client_error_code.cc)
shared_target_link_libraries(leil-client mount)

shared_add_library(leil-client-cpp client.cc client_error_code.cc)
shared_target_link_libraries(leil-client-cpp mount)

shared_target_link_libraries(leil-client ${CMAKE_DL_LIBS})

add_library(leil-client_shared SHARED client.cc leil_c_api.cc client_error_code.cc)
set_target_properties(leil-client_shared PROPERTIES OUTPUT_NAME "leil-client")
target_link_libraries(leil-client_shared ${CMAKE_DL_LIBS} mount_pic)

add_library(leilmount_shared SHARED ${MOUNT_SOURCES} sauna_client_c_linkage.cc)
target_link_libraries(leilmount_shared mount_pic ${ADDITIONAL_LIBS})
# Client dlopen()s this library into processes that already link the same mount
# code. Bind its references to its own definitions, so its globals aren't
# interposed by the caller's copies (which destroyed them twice at exit).
target_link_options(leilmount_shared PRIVATE -Wl,-Bsymbolic)

install(TARGETS leil-client DESTINATION ${LIB_SUBDIR} COMPONENT client)
install(TARGETS leil-client_pic DESTINATION ${LIB_SUBDIR} COMPONENT client)
install(TARGETS leil-client-cpp DESTINATION ${LIB_SUBDIR} COMPONENT client)
install(TARGETS leil-client-cpp_pic DESTINATION ${LIB_SUBDIR} COMPONENT client)
install(TARGETS leilmount_shared DESTINATION ${LIB_SUBDIR} COMPONENT client)
install(TARGETS leil-client_shared DESTINATION ${LIB_SUBDIR} COMPONENT client)

# Pre-rebrand library names, kept as symlinks so already deployed programs
# keep working: binaries with NEEDED libsaunafs-client.so, and clients built
# from the old static libraries, which dlopen() libsaunafsmount_shared.so by
# that name at runtime.
set(_static_prefix "${CMAKE_STATIC_LIBRARY_PREFIX}")
set(_static_suffix "${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(_shared_prefix "${CMAKE_SHARED_LIBRARY_PREFIX}")
set(_shared_suffix "${CMAKE_SHARED_LIBRARY_SUFFIX}")
foreach(_lib leil-client leil-client_pic leil-client-cpp leil-client-cpp_pic)
  string(REPLACE "leil-" "saunafs-" _legacy_lib "${_lib}")
  install_relative_symlink(${_static_prefix}${_lib}${_static_suffix}
    ${_static_prefix}${_legacy_lib}${_static_suffix} ${LIB_SUBDIR} COMPONENT client)
endforeach()
install_relative_symlink(${_shared_prefix}leil-client${_shared_suffix}
  ${_shared_prefix}saunafs-client${_shared_suffix} ${LIB_SUBDIR} COMPONENT client)
install_relative_symlink(${_shared_prefix}leilmount_shared${_shared_suffix}
  ${_shared_prefix}saunafsmount_shared${_shared_suffix} ${LIB_SUBDIR} COMPONENT client)

install(FILES leil_c_api.h DESTINATION ${INCL_SUBDIR} COMPONENT client)
install(FILES ../../errors/leil_error_codes.h DESTINATION ${INCL_SUBDIR} COMPONENT client)
# Pre-rebrand header names, kept as symlinks to the ones above.
install_relative_symlink(leil_c_api.h saunafs_c_api.h ${INCL_SUBDIR} COMPONENT client)
install_relative_symlink(leil_error_codes.h saunafs_error_codes.h ${INCL_SUBDIR} COMPONENT client)

install_legacy_dir_symlink(${INCL_PATH_LEGACY} leil COMPONENT client)
