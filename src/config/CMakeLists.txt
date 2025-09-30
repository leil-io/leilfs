collect_sources(CONFIG)

shared_add_library(config ${CONFIG_SOURCES})
shared_target_link_libraries(config slogger ${YAML_CPP_LIBRARIES})
