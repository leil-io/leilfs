configure_file(leil-cgiserver.py.in   leil-cgiserver)
configure_file(chart.cgi.in    chart.cgi)
configure_file(leil.cgi.in      leil.cgi)

add_custom_target(saunafs-cgiserver-symlink ALL
  COMMAND ln -sf leil-cgiserver saunafs-cgiserver
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/leil-cgiserver)

set(CGI_FILES err.gif favicon.ico favicon.svg index.html logomini.svg logomini.png leil.css)
set(CGI_SCRIPTS
  ${CMAKE_CURRENT_BINARY_DIR}/leil.cgi
  ${CMAKE_CURRENT_BINARY_DIR}/chart.cgi)
set(CGI_SERVERS ${CMAKE_CURRENT_BINARY_DIR}/leil-cgiserver)

install(FILES ${CGI_FILES} DESTINATION ${CGI_SUBDIR})
install(PROGRAMS ${CGI_SCRIPTS} DESTINATION ${CGI_SUBDIR})
install(PROGRAMS ${CGI_SERVERS} DESTINATION ${SBIN_SUBDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/saunafs-cgiserver DESTINATION ${SBIN_SUBDIR})
