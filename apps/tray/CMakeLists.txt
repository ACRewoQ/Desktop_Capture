# dc-tray: the resident program in the notification area (P5-B). Starts and watches mixd and capd, opens dc-ui,
# turns starting at logon on and off. Plain Win32. The decisions (restart policy, command lines, settings file,
# status file) are a portable library so the tests run on every platform.

add_library(dc_tray_core STATIC src/tray_core.cpp)
target_include_directories(dc_tray_core PUBLIC src)
dc_apply_options(dc_tray_core)
add_library(dc::tray_core ALIAS dc_tray_core)

if(NOT WIN32)
    return()
endif()

add_executable(dc-tray WIN32
    main.cpp
    src/autostart.cpp
    src/child.cpp
    src/icon.cpp
    src/win_util.cpp
)
target_link_libraries(dc-tray PRIVATE dc::tray_core dc_version shell32 user32 gdi32 advapi32 ole32 uuid)
dc_apply_options(dc-tray)
