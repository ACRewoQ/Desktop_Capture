# A3: checks recorded WAV / FLAC files (length, level, continuity of a test tone).
add_executable(dc-audio-check main.cpp)
target_link_libraries(dc-audio-check PRIVATE dc::mixd_core dc::clocksync)
dc_apply_options(dc-audio-check)
