add_executable(capd main.cpp)
target_link_libraries(capd PRIVATE dc::protocol dc::control dc::shmaudio dc_version)
dc_apply_options(capd)
