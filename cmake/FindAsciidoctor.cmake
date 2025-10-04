find_program(ASCIIDOCTOR_BINARY asciidoctor)

execute_process(
  COMMAND ruby -r rubygems -e "puts Gem.user_dir"
  OUTPUT_VARIABLE GEM_USER_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(GEM_BIN_DIR "${GEM_USER_DIR}/bin")

# First ensure asciidoctor works; install if needed
execute_process(
  COMMAND ruby -r rubygems -e "
    begin
      gem 'asciidoctor'
    rescue LoadError
      system('gem install --user-install --no-document asciidoctor') || exit(1)
    end"
  RESULT_VARIABLE GEM_INSTALL_RESULT
)
if(NOT GEM_INSTALL_RESULT EQUAL 0)
  message(FATAL_ERROR "Failed to install asciidoctor gem")
endif()

# Create a wrapper that always sets GEM_HOME and GEM_PATH properly
set(ASCIIDOCTOR_WRAPPER "${CMAKE_BINARY_DIR}/asciidoctor-wrapper.sh")
file(WRITE "${ASCIIDOCTOR_WRAPPER}" "#!/usr/bin/env bash\n")
file(APPEND "${ASCIIDOCTOR_WRAPPER}" "set -e\n")
file(APPEND "${ASCIIDOCTOR_WRAPPER}" "export GEM_HOME=\"${GEM_USER_DIR}\"\n")
file(APPEND "${ASCIIDOCTOR_WRAPPER}" "export GEM_PATH=\"${GEM_USER_DIR}\"\n")
file(APPEND "${ASCIIDOCTOR_WRAPPER}" "export PATH=\"${GEM_BIN_DIR}:$PATH\"\n")
file(APPEND "${ASCIIDOCTOR_WRAPPER}" "exec \"${GEM_BIN_DIR}/asciidoctor\" \"$@\"\n")
file(COPY "${ASCIIDOCTOR_WRAPPER}" DESTINATION "${CMAKE_BINARY_DIR}")
file(CHMOD "${ASCIIDOCTOR_WRAPPER}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ WORLD_READ)

set(ASCIIDOCTOR_BINARY "${ASCIIDOCTOR_WRAPPER}")
message(STATUS "Asciidoctor wrapper ready: ${ASCIIDOCTOR_BINARY}")
