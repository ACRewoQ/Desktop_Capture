# The engine core is a library so the tests can drive the mixer, the plan builder and the device
# manager (with fake devices) on any platform. The WASAPI backend is Windows only.
# Vendored single-file decoders for the sampler (third_party/audio_decoders), without our warnings.
add_library(dc_audio_decoders STATIC src/decoders_impl.cpp)
target_include_directories(dc_audio_decoders PRIVATE ${PROJECT_SOURCE_DIR}/third_party/audio_decoders)
if(MSVC)
    target_compile_options(dc_audio_decoders PRIVATE /W0 /utf-8)
else()
    target_compile_options(dc_audio_decoders PRIVATE -w)
endif()

add_library(dc_mixd_core STATIC
    src/audio_file.cpp
    src/control_bindings.cpp
    src/control_input_fake.cpp
    src/controller.cpp
    src/device_manager.cpp
    src/discovery.cpp
    src/dsp.cpp
    src/engine.cpp
    src/fake_backend.cpp
    src/fx/builtin.cpp
    src/fx_manager.cpp
    src/lan_peer_link.cpp
    src/lan_service.cpp
    src/lan_store.cpp
    src/mixer.cpp
    src/plan_builder.cpp
    src/profile_manager.cpp
    src/recorder.cpp
    src/sampler.cpp
    src/timer_clock.cpp
)
if(WIN32)
    target_sources(dc_mixd_core PRIVATE src/wasapi_backend.cpp src/asio_backend.cpp src/control_input_win.cpp)
    target_link_libraries(dc_mixd_core PUBLIC ole32 avrt winmm mmdevapi dnsapi ws2_32 shell32 user32)
endif()
target_include_directories(dc_mixd_core PUBLIC src)
target_link_libraries(dc_mixd_core PUBLIC dc::protocol dc::control dc::shmaudio dc::clocksync dc::lanmedia dc_version)
target_link_libraries(dc_mixd_core PRIVATE dc_audio_decoders dc_flac)
dc_apply_options(dc_mixd_core)
add_library(dc::mixd_core ALIAS dc_mixd_core)

add_executable(mixd main.cpp)
target_link_libraries(mixd PRIVATE dc::mixd_core)
dc_apply_options(mixd)

# VST3 plugins (lane A1) need JUCE; without it mixd runs with the built-in effects only.
if(DC_WITH_JUCE)
    add_subdirectory(vst3)
    target_link_libraries(mixd PRIVATE dc::mixd_vst3)
    target_compile_definitions(mixd PRIVATE DC_HAVE_VST3=1)
endif()
