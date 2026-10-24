add_library(dc_clocksync
    src/audio_fifo.cpp
    src/clock_bridge.cpp
    src/drift_controller.cpp
    src/drift_sim.cpp
    src/rate_converter.cpp
    src/resampler.cpp
    src/time_dll.cpp
    src/tone_analyzer.cpp
)
add_library(dc::clocksync ALIAS dc_clocksync)
target_include_directories(dc_clocksync PUBLIC include)
dc_apply_options(dc_clocksync)
