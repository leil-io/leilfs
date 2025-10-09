collect_sources(CONFIG)

shared_add_library(config ${CONFIG_SOURCES})
# Note: config library uses slogger functions. Since both config and slogger
# have a circular dependency at runtime, we link them together here.
# The circular dependency is resolved at the code level via slogger_interface.h
shared_target_link_libraries(config STATIC slogger MIXED ${YAML_CPP_LIBRARIES})
