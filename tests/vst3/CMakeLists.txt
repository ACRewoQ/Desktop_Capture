# mixd's VST3 host against the DC Test Gain fixture (DC_WITH_JUCE only). The test program is also
# its own scanner child, like mixd.
add_executable(dc_vst3_tests test_vst3_host.cpp)
target_link_libraries(dc_vst3_tests PRIVATE dc::mixd_vst3 Catch2::Catch2)
dc_apply_options(dc_vst3_tests)
add_dependencies(dc_vst3_tests dc_test_plugin_VST3)

set(_bundle "$<GENEX_EVAL:$<TARGET_PROPERTY:dc_test_plugin_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
set(_dir "$<PATH:GET_PARENT_PATH,${_bundle}>")
add_test(NAME vst3_host COMMAND dc_vst3_tests --plugin-dir "${_dir}" "~[crash]")
add_test(NAME vst3_host_scan_crash COMMAND dc_vst3_tests --plugin-dir "${_dir}" "[crash]")
set_tests_properties(vst3_host_scan_crash PROPERTIES ENVIRONMENT "DC_TEST_PLUGIN_CRASH=1")
set_tests_properties(vst3_host vst3_host_scan_crash PROPERTIES TIMEOUT 120)
