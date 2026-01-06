#  Copyright 2023 Leil Storage OÜ
#
#  This file is part of SaunaFS.
#
#  SaunaFS is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, version 3.
#
#  SaunaFS is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.

# Central helper to configure Windows version resource RCs
# Usage:
#   sfs_configure_version_rc(OUTPUT_PATH PRODUCT_NAME FILE_DESCRIPTION INTERNAL_NAME ORIGINAL_FILENAME)
# The function sets default version variables and runs configure_file with the shared template.

function(sfs_configure_version_rc OUTPUT_PATH PRODUCT_NAME FILE_DESCRIPTION INTERNAL_NAME ORIGINAL_FILENAME)
  if(DEFINED WINCLIENT_APP_VERSION)
    set(_WIN_APP_VER ${WINCLIENT_APP_VERSION})
  else()
    set(_WIN_APP_VER "1.0.0")
  endif()

  set(_FILE_VERSION_COMMAS "${_WIN_APP_VER},0")
  string(REPLACE "." "," _FILE_VERSION_COMMAS "${_FILE_VERSION_COMMAS}")

  set(_PRODUCT_NAME "${PRODUCT_NAME}")
  set(_FILE_DESCRIPTION "${FILE_DESCRIPTION}")
  set(_COMPANY_NAME "Leil Storage OÜ")

  if(NOT DEFINED CURRENT_YEAR)
    string(TIMESTAMP CURRENT_YEAR "%Y")
  endif()
  set(_LEGAL_COPYRIGHT "Copyright © ${_COMPANY_NAME} ${CURRENT_YEAR}")

  set(_INTERNAL_NAME "${INTERNAL_NAME}")
  set(_ORIGINAL_FILENAME "${ORIGINAL_FILENAME}")

  if(DEFINED WINCLIENT_PACKAGE_VERSION)
    set(_PRODUCT_VERSION_STR "${WINCLIENT_PACKAGE_VERSION}")
    set(_FILE_VERSION_STR "${WINCLIENT_PACKAGE_VERSION}.0")
  else()
    set(_PRODUCT_VERSION_STR "${_WIN_APP_VER}")
    set(_FILE_VERSION_STR "${_WIN_APP_VER}.0")
  endif()

  configure_file(
    ${CMAKE_SOURCE_DIR}/src/mount/windows/resources/version.rc.in
    ${OUTPUT_PATH} @ONLY)
endfunction()
