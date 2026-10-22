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

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(DcCompilerOptions)
include(DcDependencies)

configure_file(cmake/version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/dc/version.hpp" @ONLY)
add_library(dc_version INTERFACE)
target_include_directories(dc_version INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_subdirectory(libs/protocol)
add_subdirectory(libs/control)
add_subdirectory(libs/shmaudio)

add_subdirectory(apps/mixd)
add_subdirectory(apps/capd)
add_subdirectory(apps/ui)

if(DC_BUILD_TOOLS)
    add_subdirectory(tools/mock_server)
    add_subdirectory(tools/dc_cli)
    add_subdirectory(tools/shm_tone)
    if(WIN32)
        add_subdirectory(tools/v6_preview)
    endif()
endif()

if(DC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
