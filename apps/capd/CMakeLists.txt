# capd: capture process. The recording core is a static library so the tests can link it; video capture,
# composition and hardware encoding are Windows only, the audio path and the muxer build everywhere.
if(NOT DC_HAVE_FFMPEG)
    return()
endif()

add_library(capd_core STATIC
    src/settings.cpp
    src/audio_capture.cpp
    src/audio_encoder.cpp
    src/muxer.cpp
    src/recorder.cpp
    src/engine_follower.cpp
    src/avsync.cpp)
target_include_directories(capd_core PUBLIC src)
target_link_libraries(capd_core PUBLIC dc::protocol dc::control dc::shmaudio dc_ffmpeg Threads::Threads)
if(WIN32)
    target_sources(capd_core PRIVATE
        src/d3d.cpp
        src/source_wgc.cpp
        src/source_dda.cpp
        src/converter.cpp
        src/compositor.cpp
        src/video_encoder.cpp
        src/video_mix.cpp)
    target_link_libraries(capd_core PUBLIC d3d11 dxgi d3dcompiler windowsapp avrt shell32 ole32)
endif()
dc_apply_options(capd_core)
if(MSVC)
    # C4996 comes from FFmpeg's deprecated-field markers in its own headers.
    target_compile_options(capd_core PRIVATE /wd4996)
endif()

add_executable(capd main.cpp)
target_link_libraries(capd PRIVATE capd_core dc::control dc_version)
dc_apply_options(capd)
