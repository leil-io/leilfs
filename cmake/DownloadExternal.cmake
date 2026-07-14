function(download_external PCKG_NAME PCKG_DIR_NAME PCKG_URL)
  # ARGV3 - optional md5 check
  # ARGV4 - optional patch to apply
  set(${PCKG_NAME}_DIR_NAME ${PCKG_DIR_NAME} CACHE INTERNAL "" FORCE)

  if(NOT IS_DIRECTORY ${CMAKE_SOURCE_DIR}/external/${PCKG_DIR_NAME})
    message(STATUS "Downloading ${PCKG_URL}...")
    if(ARGV3)
      file(DOWNLOAD
          ${PCKG_URL}
          ${CMAKE_BINARY_DIR}/${PCKG_DIR_NAME}.zip
          INACTIVITY_TIMEOUT 15
          SHOW_PROGRESS
          STATUS DOWNLOAD_STATUS
          EXPECTED_MD5 ${ARGV3})
    else()
      file(DOWNLOAD
          ${PCKG_URL}
          ${CMAKE_BINARY_DIR}/${PCKG_DIR_NAME}.zip
          INACTIVITY_TIMEOUT 15
          SHOW_PROGRESS
          STATUS DOWNLOAD_STATUS)
    endif()

    list(GET DOWNLOAD_STATUS 0 DOWNLOAD_CODE)
    if(NOT DOWNLOAD_CODE EQUAL 0)
      list(GET DOWNLOAD_STATUS 1 DOWNLOAD_MESSAGE)
      message(FATAL_ERROR "Download ${PCKG_URL} error ${DOWNLOAD_CODE}: ${DOWNLOAD_MESSAGE}")
    endif()

    message(STATUS "Unpacking ${PCKG_DIR_NAME}.zip ...")
    execute_process(COMMAND unzip -q ${CMAKE_BINARY_DIR}/${PCKG_DIR_NAME}.zip
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/external
      RESULT_VARIABLE UNZIP_ERROR
      ERROR_VARIABLE UNZIP_ERROR_MESSAGE)
    if(NOT UNZIP_ERROR STREQUAL 0)
      message(FATAL_ERROR "unzip ${ARCHIVE_NAME} failed: ${UNZIP_ERROR} ${UNZIP_ERROR_MESSAGE}")
    endif()
    if(NOT IS_DIRECTORY ${CMAKE_SOURCE_DIR}/external/${PCKG_DIR_NAME})
      message(FATAL_ERROR "Extracting ${PCKG_DIR_NAME}.zip didn't produce directory '${PCKG_DIR_NAME}'")
    endif()
    message(STATUS "Downloading ${PCKG_NAME} finished successfully")
    if(ARGV4)
      execute_process(COMMAND patch -p1 -i ${CMAKE_SOURCE_DIR}/external/${ARGV4}.patch
                      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/external/${PCKG_DIR_NAME}
                      RESULT_VARIABLE PATCH_ERROR
                      ERROR_VARIABLE PATCH_ERROR_MESSAGE)
      if(NOT PATCH_ERROR STREQUAL 0)
        message(FATAL_ERROR "Patching ${PCKG_DIR_NAME} failed: ${PATCH_ERROR} ${PATCH_ERROR_MESSAGE}")
      endif()
    endif()
  else()
    message(STATUS "Found ${PCKG_NAME}")
  endif()
endfunction()

# Like download_external, but clones a git repository at a tag WITH its submodules
# instead of unpacking an archive. Use this when a project pins dependencies as
# git submodules (e.g. nfs-ganesha pins ntirpc): a source zip omits submodule
# contents, whereas --recurse-submodules checks each one out at its pinned commit,
# so the dependency version tracks the parent tag automatically.
function(clone_external_git PCKG_NAME PCKG_DIR_NAME GIT_URL GIT_TAG)
  set(${PCKG_NAME}_DIR_NAME ${PCKG_DIR_NAME} CACHE INTERNAL "" FORCE)
  set(_dest ${CMAKE_SOURCE_DIR}/external/${PCKG_DIR_NAME})
  if(NOT IS_DIRECTORY ${_dest})
    message(STATUS "Cloning ${GIT_URL} @ ${GIT_TAG} (with submodules)...")
    execute_process(
      COMMAND git clone --depth 1 --branch ${GIT_TAG}
              --recurse-submodules --shallow-submodules
              ${GIT_URL} ${_dest}
      RESULT_VARIABLE CLONE_ERROR
      ERROR_VARIABLE CLONE_ERROR_MESSAGE)
    if(NOT CLONE_ERROR EQUAL 0)
      message(FATAL_ERROR "git clone ${GIT_URL} failed: ${CLONE_ERROR} ${CLONE_ERROR_MESSAGE}")
    endif()
    if(NOT IS_DIRECTORY ${_dest})
      message(FATAL_ERROR "Cloning ${GIT_URL} didn't produce directory '${PCKG_DIR_NAME}'")
    endif()
    message(STATUS "Cloning ${PCKG_NAME} finished successfully")
  else()
    message(STATUS "Found ${PCKG_NAME}")
  endif()
endfunction()
