dc_juce_directory()

juce_add_console_app(dc-vst3-probe PRODUCT_NAME "dc-vst3-probe")
target_sources(dc-vst3-probe PRIVATE main.cpp)
target_compile_definitions(dc-vst3-probe PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(dc-vst3-probe PRIVATE
    juce::juce_audio_processors juce::juce_gui_basics dc_juce_options
    juce::juce_recommended_warning_flags)
if(WIN32)
    target_link_libraries(dc-vst3-probe PRIVATE psapi)
endif()
# The test plugin is the fixture for the ctest cases below.
add_dependencies(dc-vst3-probe dc_test_plugin_VST3)

if(DC_BUILD_TESTS)
    set(_bundle "$<GENEX_EVAL:$<TARGET_PROPERTY:dc_test_plugin_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
    set(_dir "$<PATH:GET_PARENT_PATH,${_bundle}>")
    # Loads the fixture, processes audio, edits parameters, restores state, opens the editor.
    add_test(NAME vst3_probe_test_plugin
        COMMAND dc-vst3-probe probe "${_bundle}" --editor-seconds 1)
    set_tests_properties(vst3_probe_test_plugin PROPERTIES ENVIRONMENT "DC_TEST_PLUGIN_GUI_EDITS=1")
    # A plugin that crashes or hangs while loading must not take the scanner down with it.
    add_test(NAME vst3_scan_survives_crash
        COMMAND dc-vst3-probe scan --dir "${_dir}" --editor-seconds 0)
    set_tests_properties(vst3_scan_survives_crash PROPERTIES
        ENVIRONMENT "DC_TEST_PLUGIN_CRASH=1" PASS_REGULAR_EXPRESSION "crashed/hung 1")
    add_test(NAME vst3_scan_survives_hang
        COMMAND dc-vst3-probe scan --dir "${_dir}" --editor-seconds 0 --timeout 5)
    set_tests_properties(vst3_scan_survives_hang PROPERTIES
        ENVIRONMENT "DC_TEST_PLUGIN_HANG=1" PASS_REGULAR_EXPRESSION "HUNG.*timed out" TIMEOUT 60)
endif()
