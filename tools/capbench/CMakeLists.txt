# dc-capbench: P1-V3 verification tool (desktop capture + hardware encoding on the GPU). Windows only.
add_executable(dc-capbench
    main.cpp
    d3d.cpp
    source_wgc.cpp
    source_dda.cpp
    converter.cpp
    encoder.cpp
    metrics.cpp
    pattern.cpp)
target_link_libraries(dc-capbench PRIVATE dc_ffmpeg dc_version d3d11 dxgi windowsapp pdh psapi)
dc_apply_options(dc-capbench)
if(MSVC)
    # C4996 comes from FFmpeg's deprecated-field markers in its own headers.
    target_compile_options(dc-capbench PRIVATE /wd4996)
endif()
