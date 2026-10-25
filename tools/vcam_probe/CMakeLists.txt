# dc-vcam-probe: opens a camera like a camera app (Media Foundation or DirectShow) and measures fps, drops and
# latency from the stamp of "capd vcam --test-pattern". Windows only.
add_executable(dc-vcam-probe vcam_probe.cpp)
target_link_libraries(dc-vcam-probe PRIVATE dc_vcam_headers mfplat mf mfreadwrite mfuuid strmiids ole32 oleaut32)
dc_apply_options(dc-vcam-probe)
