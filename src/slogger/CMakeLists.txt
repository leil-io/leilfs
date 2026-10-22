collect_sources(SLOGGER)

shared_add_library(slogger ${SLOGGER_SOURCES})
if(MINGW)
    shared_target_link_libraries(slogger fmt::fmt)
else()
    shared_target_link_libraries(slogger fmt::fmt spdlog::spdlog)
endif()
shared_target_link_libraries(slogger STATIC config sfserr SHARED config_pic sfserr_pic)
