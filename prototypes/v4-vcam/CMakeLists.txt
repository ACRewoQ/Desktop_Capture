# V4 technical verification: virtual camera prototypes.
# Standalone project (not part of the main build) so it can be built and thrown away freely:
#   cmake -S prototypes/v4-vcam -B build/v4-vcam -A x64
#   cmake --build build/v4-vcam --config Release
# or simply run scripts/v4-build.ps1 inside this folder.
cmake_minimum_required(VERSION 3.25)
project(DcV4VirtualCamera LANGUAGES CXX)

# Pattern round-trip test runs everywhere (no Windows API).
add_executable(vcam-pattern-test tests/pattern_test.cpp)
target_compile_features(vcam-pattern-test PRIVATE cxx_std_20)

if(NOT WIN32)
    message(STATUS "V4: not Windows, only the pattern test is built")
    return()
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # self-contained DLLs
add_compile_definitions(UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)

function(v4_strict target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    # Not global: BaseClasses needs the min macro and softcam needs mmsystem from windows.h.
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endfunction()

# --- Media Foundation virtual camera (Windows 11) ---------------------------------------------
add_library(vcam_mf SHARED mf/vcam_source.cpp mf/vcam_mf.def)
v4_strict(vcam_mf)
target_link_libraries(vcam_mf PRIVATE mfplat mf mfuuid mfsensorgroup ole32 advapi32)

add_executable(vcam-mf-ctl mf/vcam_mf_ctl.cpp)
v4_strict(vcam-mf-ctl)
target_link_libraries(vcam-mf-ctl PRIVATE mfplat mfsensorgroup ole32 delayimp)
target_link_options(vcam-mf-ctl PRIVATE /DELAYLOAD:mfsensorgroup.dll)

# --- DirectShow virtual camera (Windows 10) on top of softcam ----------------------------------
include(FetchContent)
FetchContent_Declare(softcam
    GIT_REPOSITORY https://github.com/tshino/softcam.git
    GIT_TAG 8f545e2a3faa2c0d6e6e05378f945ee2a7adfc1d)
FetchContent_MakeAvailable(softcam)
set(SC "${softcam_SOURCE_DIR}/src")

# Own shared-memory names, so this filter never pairs with a stock softcam installation.
file(READ "${SC}/softcamcore/FrameBuffer.cpp" _fb)
string(REPLACE "DirectShow Softcam/" "DesktopCapture.VCamDS.v1/" _fb "${_fb}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/patched/FrameBuffer.cpp" "${_fb}")

add_library(vcam_ds SHARED
    ds/vcam_ds.cpp ds/vcam_ds.def
    ${SC}/baseclasses/amfilter.cpp ${SC}/baseclasses/amvideo.cpp ${SC}/baseclasses/combase.cpp
    ${SC}/baseclasses/dllentry.cpp ${SC}/baseclasses/dllsetup.cpp ${SC}/baseclasses/mtype.cpp
    ${SC}/baseclasses/source.cpp ${SC}/baseclasses/winutil.cpp ${SC}/baseclasses/wxdebug.cpp
    ${SC}/baseclasses/wxlist.cpp ${SC}/baseclasses/wxutil.cpp
    ${SC}/softcamcore/DShowSoftcam.cpp ${SC}/softcamcore/Misc.cpp ${SC}/softcamcore/SenderAPI.cpp
    ${SC}/softcamcore/Watchdog.cpp "${CMAKE_CURRENT_BINARY_DIR}/patched/FrameBuffer.cpp")
# softcam / BaseClasses are older code: C++17 and permissive mode, default warnings.
target_compile_features(vcam_ds PRIVATE cxx_std_17)
target_compile_options(vcam_ds PRIVATE /permissive /EHsc /W1)
target_include_directories(vcam_ds PRIVATE ${SC} ${SC}/baseclasses ${SC}/softcamcore)
target_link_libraries(vcam_ds PRIVATE strmiids winmm ole32 oleaut32 uuid advapi32 user32 gdi32)

# --- Producer and probe ------------------------------------------------------------------------
add_executable(vcam-feed feed/vcam_feed.cpp)
v4_strict(vcam-feed)
target_link_libraries(vcam-feed PRIVATE winmm)

add_executable(vcam-probe probe/vcam_probe.cpp)
v4_strict(vcam-probe)
target_link_libraries(vcam-probe PRIVATE mfplat mf mfreadwrite mfuuid strmiids ole32 oleaut32)

# Everything next to each other: vcam-feed loads vcam_ds.dll from its own folder.
# The $<1:...> genex keeps multi-config generators (Visual Studio) from adding a Release subfolder.
set_target_properties(vcam_mf vcam_ds vcam-mf-ctl vcam-feed vcam-probe vcam-pattern-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>")
