add_executable(dc-drift-sim main.cpp)
target_link_libraries(dc-drift-sim PRIVATE dc::clocksync)
dc_apply_options(dc-drift-sim)
