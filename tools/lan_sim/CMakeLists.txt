add_executable(dc-lan-sim main.cpp)
target_link_libraries(dc-lan-sim PRIVATE dc::lanmedia)
dc_apply_options(dc-lan-sim)
