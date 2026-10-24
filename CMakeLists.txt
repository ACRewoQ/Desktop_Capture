cmake_minimum_required(VERSION 3.25)

project(DesktopCapture
    VERSION 0.1.0
    DESCRIPTION "Software mixer with desktop capture and remote controller"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DC_BUILD_TESTS "Build unit and integration tests" ON)
option(DC_BUILD_TOOLS "Build developer tools (mock server, CLI client, shm tone generator)" ON)
option(DC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(DC_WITH_JUCE "Fetch JUCE and build the JUCE-based targets (V7 VST3 host probe)" OFF)

if(DC_BUILD_TESTS)
    enable_testing() # before the tool directories, which register their own tests
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(DcCompilerOptions)
include(DcDependencies)
if(DC_WITH_JUCE)
    include(DcJuce)
endif()

configure_file(cmake/version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/dc/version.hpp" @ONLY)
add_library(dc_version INTERFACE)
target_include_directories(dc_version INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_subdirectory(libs/protocol)
add_subdirectory(libs/control)
add_subdirectory(libs/shmaudio)
add_subdirectory(libs/clocksync)
add_subdirectory(libs/lanmedia)

add_subdirectory(apps/mixd)
add_subdirectory(apps/capd)
add_subdirectory(apps/ui)

if(DC_BUILD_TOOLS)
    add_subdirectory(tools/mock_server)
    add_subdirectory(tools/dc_cli)
    add_subdirectory(tools/shm_tone)
    add_subdirectory(tools/drift_sim)
    add_subdirectory(tools/lan_sim)
    add_subdirectory(tools/lan_tone)
    add_subdirectory(tools/audio_check)
    if(WIN32)
        add_subdirectory(tools/capbench)
        add_subdirectory(tools/v6_preview)
        add_subdirectory(tools/loopback_probe)
        add_subdirectory(tools/drift_test)
        add_subdirectory(tools/engine_check)
    endif()
    if(DC_WITH_JUCE)
        add_subdirectory(tools/vst3_test_plugin)
        add_subdirectory(tools/vst3_probe)
    endif()
    if(DC_HAVE_QT)
        add_subdirectory(tools/v5_remote_probe)
    endif()
endif()

if(DC_BUILD_TESTS)
    add_subdirectory(tests)
endif()
