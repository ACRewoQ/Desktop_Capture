# P1 verification V1: per-application audio capture (WASAPI process loopback). Windows only.
add_executable(dc-loopback-probe main.cpp loopback_capture.cpp tone.cpp)
target_link_libraries(dc-loopback-probe PRIVATE ole32 avrt mmdevapi)
dc_apply_options(dc-loopback-probe)
# Static CRT: the probe has no vcpkg dependencies, and this lets the exe run on a test PC
# (e.g. the Windows 10 machine) without Visual Studio or the VC++ runtime installed.
set_property(TARGET dc-loopback-probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
