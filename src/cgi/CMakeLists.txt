configure_file(cgiserv.py.in   sfscgiserv)
configure_file(saunafs-cgiserver.py.in   saunafs-cgiserver)
configure_file(chart.cgi.in    chart.cgi)
configure_file(sfs.cgi.in      sfs.cgi)

set(CGI_FILES err.gif favicon.ico favicon.svg index.html logomini.svg logomini.png sfs.css)
set(CGI_SCRIPTS
    ${CMAKE_CURRENT_BINARY_DIR}/sfs.cgi
    ${CMAKE_CURRENT_BINARY_DIR}/chart.cgi)
set(CGI_SERVERS
    ${CMAKE_CURRENT_BINARY_DIR}/sfscgiserv
    ${CMAKE_CURRENT_BINARY_DIR}/saunafs-cgiserver)

install(FILES ${CGI_FILES} DESTINATION ${CGI_SUBDIR})
install(PROGRAMS ${CGI_SCRIPTS} DESTINATION ${CGI_SUBDIR})
install(PROGRAMS ${CGI_SERVERS} DESTINATION ${SBIN_SUBDIR})
