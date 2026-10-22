add_executable(dc-drift-test main.cpp)
target_link_libraries(dc-drift-test PRIVATE dc::clocksync ole32 avrt)
dc_apply_options(dc-drift-test)
