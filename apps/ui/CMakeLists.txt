add_executable(dc-ui main.cpp)
target_link_libraries(dc-ui PRIVATE dc::protocol dc_version)
dc_apply_options(dc-ui)
