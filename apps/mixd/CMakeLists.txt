add_executable(mixd main.cpp)
target_link_libraries(mixd PRIVATE dc::protocol dc::control dc::shmaudio dc_version)
dc_apply_options(mixd)
