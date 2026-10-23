# DC Test Gain: a tiny VST3 plugin built from source so the V7 probe has a known fixture.
# Environment switches (read when the processor is created) let tests exercise the failure paths:
#   DC_TEST_PLUGIN_CRASH=1      access violation while the plugin is created
#   DC_TEST_PLUGIN_HANG=1       never returns from creation
#   DC_TEST_PLUGIN_GUI_EDITS=1  the open editor moves "Gain" by itself, like a user dragging a knob
dc_juce_directory()

juce_add_plugin(dc_test_plugin
    PRODUCT_NAME "DC Test Gain"
    COMPANY_NAME "DesktopCapture"
    BUNDLE_ID "jp.mimicnet.desktopcapture.testgain"
    PLUGIN_MANUFACTURER_CODE Dcap
    PLUGIN_CODE Dtgn
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Dynamics
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(dc_test_plugin PRIVATE plugin.cpp)
target_compile_definitions(dc_test_plugin PUBLIC JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(dc_test_plugin
    PRIVATE juce::juce_audio_utils dc_juce_options
    PUBLIC juce::juce_recommended_warning_flags)
