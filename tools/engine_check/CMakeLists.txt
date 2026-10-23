# G2: checks the engine's output for dropouts by analysing a test tone in the device's loopback.
add_executable(dc-engine-check main.cpp)
target_link_libraries(dc-engine-check PRIVATE dc::clocksync ole32)
dc_apply_options(dc-engine-check)
