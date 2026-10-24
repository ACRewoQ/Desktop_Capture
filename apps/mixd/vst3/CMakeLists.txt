# VST3 hosting for mixd (JUCE 9). JUCE sets up <windows.h> itself, hence its own directory.
dc_juce_directory()

add_library(dc_mixd_vst3 STATIC vst3_host.cpp)
target_include_directories(dc_mixd_vst3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(dc_mixd_vst3 PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
target_link_libraries(dc_mixd_vst3
    PUBLIC dc::mixd_core
    PRIVATE juce::juce_audio_processors juce::juce_gui_basics dc_juce_options)
add_library(dc::mixd_vst3 ALIAS dc_mixd_vst3)
