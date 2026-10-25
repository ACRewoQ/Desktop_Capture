# Media Foundation H.264 encode/decode and the D3D11 frame path (decode device -> shared textures ->
# display device), Windows only. Measured in P1/V6 (tools/v6_preview); the remote's preview in dc-ui
# uses the same decode path (docs/ui.md).
add_library(dc_mfvideo STATIC
    src/common.cpp
    src/d3d_video.cpp
    src/guids.cpp
    src/mf_codec.cpp
)
add_library(dc::mfvideo ALIAS dc_mfvideo)
target_include_directories(dc_mfvideo PUBLIC include)
target_link_libraries(dc_mfvideo PUBLIC mfplat mfuuid strmiids d3d11 dxgi ole32 oleaut32 dbghelp)
dc_apply_options(dc_mfvideo)
if(MSVC)
    # PDBs in Release too, so the crash reporter can print function names and lines.
    target_compile_options(dc_mfvideo PRIVATE $<$<CONFIG:Release>:/Zi>)
endif()
