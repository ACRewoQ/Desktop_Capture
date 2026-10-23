# JUCE (AGPLv3, fine for this personal project). Fetched with FetchContent instead of vcpkg:
# JUCE's own CMake API (juce_add_plugin, juce_add_console_app, juceaide) needs its source tree.
# Point FETCHCONTENT_SOURCE_DIR_JUCE at a local checkout to build offline.

include(FetchContent)
enable_language(C) # JUCE compiles a few C sources (e.g. its bundled libraries)

set(DC_JUCE_VERSION 9.0.4)
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${DC_JUCE_VERSION}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(juce)

# Settings shared by every target that compiles JUCE modules. JUCE sets up <windows.h> itself,
# so targets using it should live in a directory that calls dc_juce_directory() to drop the
# project-wide WIN32_LEAN_AND_MEAN / UNICODE definitions from DcCompilerOptions.
add_library(dc_juce_options INTERFACE)
target_compile_definitions(dc_juce_options INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)
target_link_libraries(dc_juce_options INTERFACE
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags)
if(MSVC)
    target_compile_options(dc_juce_options INTERFACE /utf-8 /Zc:__cplusplus /bigobj)
endif()

macro(dc_juce_directory)
    set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "")
endmacro()
