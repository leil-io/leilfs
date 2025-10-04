#------------------------------------------------------------------------------
# Asciidoctor bootstrap (self-contained, explicit, minimal)
#------------------------------------------------------------------------------
message(STATUS "Checking for asciidoctor (auto-setup: ${ASCIIDOCTOR_AUTO_SETUP})")

# 1. Try system binary first
find_program(ASCIIDOCTOR_BINARY asciidoctor)
if(ASCIIDOCTOR_BINARY)
    execute_process(COMMAND "${ASCIIDOCTOR_BINARY}" --version
            RESULT_VARIABLE _ok OUTPUT_QUIET ERROR_QUIET)
    if(_ok EQUAL 0)
        message(STATUS "Using system asciidoctor: ${ASCIIDOCTOR_BINARY}")
        return()
    endif()
endif()

# 2. Allow automatic user-level installation of Asciidoctor (disable for reproducible or offline builds)
if(NOT ASCIIDOCTOR_AUTO_SETUP)
    message(FATAL_ERROR "asciidoctor not found or broken (ASCIIDOCTOR_AUTO_SETUP=OFF)")
endif()

# 3. Install gem under user directory
execute_process(
        COMMAND ruby -r rubygems -e "
      begin
        gem 'asciidoctor', '= ${ASCIIDOCTOR_VERSION}'
      rescue Gem::LoadError
        system('gem install --user-install --no-document asciidoctor -v ${ASCIIDOCTOR_VERSION}') || exit(1)
      end"
        RESULT_VARIABLE _gem_ok
)
if(NOT _gem_ok EQUAL 0)
    message(FATAL_ERROR "Failed to install asciidoctor gem ${ASCIIDOCTOR_VERSION}")
endif()

# 4. Get setup info
execute_process(
        COMMAND ruby -r rubygems -e "puts Gem.user_dir"
        OUTPUT_VARIABLE GEM_USER_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(GEM_BIN_DIR "${GEM_USER_DIR}/bin")

# 5. Create a wrapper that always sets GEM_HOME and GEM_PATH properly
set(ASCIIDOCTOR_WRAPPER "${CMAKE_BINARY_DIR}/asciidoctor-wrapper.sh")
file(WRITE "${ASCIIDOCTOR_WRAPPER}" "#!/usr/bin/env bash\n")
file(APPEND "${ASCIIDOCTOR_WRAPPER}" "set -e\n")
file(APPEND "${ASCIIDOCTOR_WRAPPER}" "export GEM_HOME=\"${GEM_USER_DIR}\"\n")
file(APPEND "${ASCIIDOCTOR_WRAPPER}" "export GEM_PATH=\"${GEM_USER_DIR}\"\n")
file(APPEND "${ASCIIDOCTOR_WRAPPER}" "export PATH=\"${GEM_BIN_DIR}:$PATH\"\n")
file(APPEND "${ASCIIDOCTOR_WRAPPER}" "exec \"${GEM_BIN_DIR}/asciidoctor\" \"$@\"\n")
# Wrapper is only used inside build tree; no need for world execution.
file(CHMOD "${ASCIIDOCTOR_WRAPPER}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ WORLD_READ)

set(ASCIIDOCTOR_BINARY "${ASCIIDOCTOR_WRAPPER}")
message(STATUS "Asciidoctor ready: ${ASCIIDOCTOR_BINARY} (v${ASCIIDOCTOR_VERSION})")
