# V6 (phase 1 verification): preview playback latency.
# Media Foundation H.264 decoding (DXVA or software) -> D3D11 -> Qt Quick, measured per stage.
# See docs/verification/v6-preview-playback.md.

add_library(dc_v6core STATIC
    src/common.cpp
    src/d3d_video.cpp
    src/guids.cpp
    src/mf_codec.cpp
    src/pipeline.cpp
    src/stats.cpp
    src/test_pattern.cpp
)
target_include_directories(dc_v6core PUBLIC src)
target_link_libraries(dc_v6core PUBLIC dc_asio mfplat mfuuid strmiids d3d11 dxgi winmm ole32 oleaut32)
dc_apply_options(dc_v6core)

add_executable(dc-v6-bench src/bench_main.cpp)
target_link_libraries(dc-v6-bench PRIVATE dc_v6core)
dc_apply_options(dc-v6-bench)

if(DC_HAVE_QT)
    qt_standard_project_setup(REQUIRES 6.5)
    qt_add_executable(dc-v6-preview src/preview_main.cpp src/video_item.cpp src/video_item.hpp)
    qt_add_qml_module(dc-v6-preview
        URI DcV6
        VERSION 1.0
        QML_FILES qml/Main.qml
    )
    target_link_libraries(dc-v6-preview PRIVATE dc_v6core Qt6::Quick)
    dc_apply_options(dc-v6-preview)
    # Console subsystem on purpose: the report goes to stdout.
    set_target_properties(dc-v6-preview PROPERTIES WIN32_EXECUTABLE OFF)
    # Copy the Qt runtime next to the exe so it starts without PATH changes.
    find_program(DC_WINDEPLOYQT windeployqt HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
    if(DC_WINDEPLOYQT)
        add_custom_command(TARGET dc-v6-preview POST_BUILD
            COMMAND "${DC_WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml" "$<TARGET_FILE:dc-v6-preview>"
            VERBATIM)
    endif()
endif()
