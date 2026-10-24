# Virtual camera (P3 lane B2, from the V4 verification in prototypes/v4-vcam):
#   vcam_mf.dll      Media Foundation camera source, loaded by the Windows Camera Frame Server (Windows 11)
#   vcam_ds.dll      DirectShow source filter on softcam (Windows 10)
#   dc-vcam-ctl      creates / removes the MF camera for the current user
#   dc_vcam_sender   producer side, linked into capd
# Installed by scripts/vcam-register.ps1 (copies the DLLs to %ProgramData%, registers them; one UAC prompt).
# The pixel helpers (include/dc/vcam/pixels.hpp) are portable and tested on every platform.
add_library(dc_vcam_headers INTERFACE)
target_include_directories(dc_vcam_headers INTERFACE include)

if(NOT WIN32)
    return()
endif()

# BaseClasses (DirectShow) needs the min/max macros and mmsystem that the project-wide NOMINMAX /
# WIN32_LEAN_AND_MEAN take away: drop them for this folder and give them back to our own targets.
get_directory_property(_defs COMPILE_DEFINITIONS)
list(REMOVE_ITEM _defs NOMINMAX WIN32_LEAN_AND_MEAN)
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_defs}")
target_compile_definitions(dc_vcam_headers INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)

# The camera DLLs run inside other processes (the Frame Server service, camera apps): link the C runtime
# statically so they carry no dependency on our install folder.
set(DC_VCAM_STATIC_CRT "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- producer (capd) ---
add_library(dc_vcam_sender STATIC sender/sender.cpp)
target_link_libraries(dc_vcam_sender PUBLIC dc_vcam_headers)
dc_apply_options(dc_vcam_sender)

# --- Media Foundation camera (Windows 11) ---
add_library(vcam_mf SHARED mf/source.cpp mf/vcam_mf.def)
target_link_libraries(vcam_mf PRIVATE dc_vcam_headers mfplat mf mfuuid mfsensorgroup ole32 advapi32)
set_target_properties(vcam_mf PROPERTIES MSVC_RUNTIME_LIBRARY "${DC_VCAM_STATIC_CRT}")
dc_apply_options(vcam_mf)

add_executable(dc-vcam-ctl ctl/vcam_ctl.cpp)
target_link_libraries(dc-vcam-ctl PRIVATE dc_vcam_headers mfplat mfsensorgroup ole32 delayimp)
# Delay-loaded: Windows 10's mfsensorgroup.dll has no MFCreateVirtualCamera, and the tool should say so.
target_link_options(dc-vcam-ctl PRIVATE /DELAYLOAD:mfsensorgroup.dll)
dc_apply_options(dc-vcam-ctl)

# --- DirectShow camera (Windows 10) on top of softcam (MIT) ---
include(FetchContent)
# Only the sources are used: SOURCE_SUBDIR points at a folder without a CMakeLists.txt, so softcam's own project
# (its library and sample apps) is not added to the build.
FetchContent_Declare(softcam
    GIT_REPOSITORY https://github.com/tshino/softcam.git
    GIT_TAG 8f545e2a3faa2c0d6e6e05378f945ee2a7adfc1d
    SOURCE_SUBDIR dc-sources-only)
FetchContent_MakeAvailable(softcam)
set(SC "${softcam_SOURCE_DIR}/src")

# Own shared-memory names, so this filter never pairs with a stock softcam installation.
file(READ "${SC}/softcamcore/FrameBuffer.cpp" _fb)
string(REPLACE "DirectShow Softcam/" "DesktopCapture.VCamDS.v1/" _fb "${_fb}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/patched/FrameBuffer.cpp" "${_fb}")

# Stock softcam offers no format while no sender runs, so apps fail to open it (and Chrome hides it, V4 finding
# 8). Always offer 1920x1080 30 fps and send a dark "no signal" colour until a sender of that size appears.
function(dc_vcam_patch var from to)
    string(FIND "${${var}}" "${from}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "vcam: softcam patch target not found: ${from}")
    endif()
    string(REPLACE "${from}" "${to}" _out "${${var}}")
    set(${var} "${_out}" PARENT_SCOPE)
endfunction()
file(READ "${SC}/softcamcore/DShowSoftcam.cpp" _ds)
dc_vcam_patch(_ds "m_valid(m_frame_buffer ? true : false)" "m_valid(true)")
dc_vcam_patch(_ds "m_width(m_frame_buffer.width())" "m_width(m_frame_buffer ? m_frame_buffer.width() : 1920)")
dc_vcam_patch(_ds "m_height(m_frame_buffer.height())" "m_height(m_frame_buffer ? m_frame_buffer.height() : 1080)")
dc_vcam_patch(_ds "m_framerate(m_frame_buffer.framerate())" "m_framerate(m_frame_buffer ? m_frame_buffer.framerate() : 30.0f)")
dc_vcam_patch(_ds "Timer::sleep(0.100f);" "Timer::sleep(1.0f / 30);")
dc_vcam_patch(_ds "std::memcpy(pData, m_screenshot.get(), size);"
    "if (m_screenshot) std::memcpy(pData, m_screenshot.get(), size); else for (std::size_t i = 0; i + 2 < size; i += 3) { pData[i] = 0x30; pData[i + 1] = 0x20; pData[i + 2] = 0x20; }")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/patched/DShowSoftcam.cpp" "${_ds}")

add_library(vcam_ds SHARED
    ds/vcam_ds.cpp ds/vcam_ds.def
    ${SC}/baseclasses/amfilter.cpp ${SC}/baseclasses/amvideo.cpp ${SC}/baseclasses/combase.cpp
    ${SC}/baseclasses/dllentry.cpp ${SC}/baseclasses/dllsetup.cpp ${SC}/baseclasses/mtype.cpp
    ${SC}/baseclasses/source.cpp ${SC}/baseclasses/winutil.cpp ${SC}/baseclasses/wxdebug.cpp
    ${SC}/baseclasses/wxlist.cpp ${SC}/baseclasses/wxutil.cpp
    ${SC}/softcamcore/Misc.cpp ${SC}/softcamcore/SenderAPI.cpp
    ${SC}/softcamcore/Watchdog.cpp "${CMAKE_CURRENT_BINARY_DIR}/patched/FrameBuffer.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/patched/DShowSoftcam.cpp")
# softcam / BaseClasses are older code: C++17, permissive mode, default warnings.
target_compile_features(vcam_ds PRIVATE cxx_std_17)
set_target_properties(vcam_ds PROPERTIES CXX_STANDARD 17 MSVC_RUNTIME_LIBRARY "${DC_VCAM_STATIC_CRT}")
target_compile_options(vcam_ds PRIVATE /permissive /EHsc /W1 /utf-8)
target_include_directories(vcam_ds PRIVATE ${SC} ${SC}/baseclasses ${SC}/softcamcore)
target_include_directories(vcam_ds PRIVATE include)
target_link_libraries(vcam_ds PRIVATE strmiids winmm ole32 oleaut32 uuid advapi32 user32 gdi32)

# The DLLs and the control tool side by side, so the register script copies one folder.
set_target_properties(vcam_mf vcam_ds dc-vcam-ctl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/vcam>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/vcam>")
