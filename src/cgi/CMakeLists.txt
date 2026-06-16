configure_file(leil-cgiserver.py.in   leil-cgiserver)
configure_file(chart.cgi.in    chart.cgi)
configure_file(leil.cgi.in      leil.cgi)

add_custom_target(saunafs-cgiserver-symlink ALL
  COMMAND ${CMAKE_COMMAND} -E create_symlink leil-cgiserver saunafs-cgiserver
  COMMAND ${CMAKE_COMMAND} -E create_symlink leil.cgi sfs.cgi
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS
    ${CMAKE_CURRENT_BINARY_DIR}/leil-cgiserver
    ${CMAKE_CURRENT_BINARY_DIR}/leil.cgi)

set(CGI_FILES err.gif favicon.ico favicon.svg index.html logomini.svg logomini.png leil.css)
set(CGI_SCRIPTS
  ${CMAKE_CURRENT_BINARY_DIR}/leil.cgi
  ${CMAKE_CURRENT_BINARY_DIR}/chart.cgi)
set(CGI_SERVERS ${CMAKE_CURRENT_BINARY_DIR}/leil-cgiserver)

function(install_symlink target link_name destination)
  install(CODE "
    execute_process(
      COMMAND \"${CMAKE_COMMAND}\" -E create_symlink
        \"${target}\"
        \"\$ENV{DESTDIR}${destination}/${link_name}\")
  ")
endfunction()

install(FILES ${CGI_FILES} DESTINATION ${CGI_SUBDIR})
install(PROGRAMS ${CGI_SCRIPTS} DESTINATION ${CGI_SUBDIR})

# Create symlinks for legacy CGI compatibility.
install_symlink(leil.cgi sfs.cgi ${CGI_SUBDIR})
install_symlink(leil.css sfs.css ${CGI_SUBDIR})
install_symlink(leil-cgi sfscgi ${CMAKE_INSTALL_FULL_DATAROOTDIR})

install(PROGRAMS ${CGI_SERVERS} DESTINATION ${SBIN_SUBDIR})
install_symlink(leil-cgiserver saunafs-cgiserver ${SBIN_SUBDIR})
