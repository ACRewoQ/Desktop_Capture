# P1 verification V1: per-application audio capture (WASAPI process loopback). Windows only.
add_executable(dc-loopback-probe main.cpp loopback_capture.cpp tone.cpp)
target_link_libraries(dc-loopback-probe PRIVATE ole32 avrt mmdevapi)
dc_apply_options(dc-loopback-probe)
