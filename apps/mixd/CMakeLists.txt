# The engine core is a library so the tests can drive the mixer, the plan builder and the device
# manager (with fake devices) on any platform. The WASAPI backend is Windows only.
add_library(dc_mixd_core STATIC
    src/controller.cpp
    src/device_manager.cpp
    src/dsp.cpp
    src/engine.cpp
    src/fake_backend.cpp
    src/fx/builtin.cpp
    src/fx_manager.cpp
    src/mixer.cpp
    src/plan_builder.cpp
    src/timer_clock.cpp
)
if(WIN32)
    target_sources(dc_mixd_core PRIVATE src/wasapi_backend.cpp)
    target_link_libraries(dc_mixd_core PUBLIC ole32 avrt winmm)
endif()
target_include_directories(dc_mixd_core PUBLIC src)
target_link_libraries(dc_mixd_core PUBLIC dc::protocol dc::control dc::shmaudio dc::clocksync dc_version)
dc_apply_options(dc_mixd_core)
add_library(dc::mixd_core ALIAS dc_mixd_core)

add_executable(mixd main.cpp)
target_link_libraries(mixd PRIVATE dc::mixd_core)
dc_apply_options(mixd)
