add_executable(dc-cli main.cpp)
target_link_libraries(dc-cli PRIVATE dc::control dc_version)
dc_apply_options(dc-cli)
