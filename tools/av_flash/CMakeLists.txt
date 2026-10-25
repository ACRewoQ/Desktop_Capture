# G2: flashes a corner of the screen in step with mixd --test-pulse, for the A/V sync check of recordings
# (capd avsync).
add_executable(dc-av-flash main.cpp)
target_link_libraries(dc-av-flash PRIVATE winmm gdi32 user32)
dc_apply_options(dc-av-flash)
