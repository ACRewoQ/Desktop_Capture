# dc-ui: the mixer UI (local PC and remote controller). Qt 6 Quick with plain items and pointer
# handlers (P1/V5). Without Qt a placeholder executable is built so that the three-process layout and
# scripts/test.ps1 still work.

if(NOT DC_HAVE_QT)
    add_executable(dc-ui main_noqt.cpp)
    target_link_libraries(dc-ui PRIVATE dc::protocol dc_version)
    dc_apply_options(dc-ui)
    return()
endif()

find_package(Qt6 REQUIRED COMPONENTS Quick Qml Network)
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

# Everything except the screen: protocol client, models, self test. Also linked by tests/ui.
add_library(dc_ui_core STATIC
    src/audio_out.hpp
    src/audio_out_null.cpp
    src/connection.cpp
    src/connection.hpp
    src/exercise.cpp
    src/exercise.hpp
    src/monitor_audio.cpp
    src/monitor_audio.hpp
    src/param_hub.cpp
    src/param_hub.hpp
    src/preview_stream.cpp
    src/preview_stream.hpp
    src/recorder.cpp
    src/recorder.hpp
    src/remote_check.cpp
    src/remote_check.hpp
    src/route_choices.cpp
    src/route_choices.hpp
    src/selftest.cpp
    src/selftest.hpp
    src/strip_model.cpp
    src/strip_model.hpp
)
add_library(dc::ui_core ALIAS dc_ui_core)
set_target_properties(dc_ui_core PROPERTIES AUTOMOC ON)
target_include_directories(dc_ui_core PUBLIC src)
target_link_libraries(dc_ui_core PUBLIC Qt6::Core Qt6::Network Qt6::Qml dc::protocol dc::lanmedia)
if(WIN32)
    # The monitor audio's output (WASAPI).
    target_sources(dc_ui_core PRIVATE src/audio_out_wasapi.cpp)
    target_link_libraries(dc_ui_core PRIVATE ole32)
endif()
dc_apply_options(dc_ui_core)

qt_add_executable(dc-ui src/main.cpp src/preview_video.cpp src/preview_video.hpp src/touch_check.cpp src/touch_check.hpp
                  src/measure.cpp src/measure.hpp)
set_target_properties(dc-ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE TRUE)
# QRhi (the preview) and QWindowSystemInterface (the self test's synthetic touch) live in the private
# Gui module.
if(NOT TARGET Qt6::GuiPrivate)  # Qt 6.9+ has it as a component of its own; older Qt with Gui
    find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
endif()
target_link_libraries(dc-ui PRIVATE Qt6::GuiPrivate)
if(WIN32)
    # The preview's decoder (Media Foundation, as in P1/V6).
    target_sources(dc-ui PRIVATE src/preview_decoder.cpp src/preview_decoder.hpp)
    target_link_libraries(dc-ui PRIVATE dc::mfvideo psapi)
endif()

set(DC_UI_QML
    Main.qml
    Theme.qml
    Sheets.qml
    TopBar.qml
    NavRail.qml
    RecordButton.qml
    MonitorButton.qml
    PairingPanel.qml
    # pages
    MixerPage.qml
    SettingsPage.qml
    VideoPage.qml
    CapturePage.qml
    SamplerPage.qml
    LanPage.qml
    ControlPage.qml
    ProfilePage.qml
    # mixer
    Strip.qml
    Fader.qml
    Meter.qml
    PanControl.qml
    FilterButton.qml
    ToggleButton.qml
    AddTrackTile.qml
    TrackPanel.qml
    MasterPanel.qml
    TargetList.qml
    FxPanel.qml
    FxEditor.qml
    SourcePicker.qml
    # video
    Video.qml
    VideoScenes.qml
    VideoSources.qml
    VideoOutputs.qml
    VideoPreview.qml
    LayerCanvas.qml
    LayerSheet.qml
    SourceSheet.qml
    # sampler
    PadSheet.qml
    # hotkeys and MIDI
    Bindings.qml
    BindingSheet.qml
    # shared parts
    MenuButton.qml
    Toast.qml
    Sheet.qml
    ChoiceSheet.qml
    NumberSheet.qml
    TextSheet.qml
    SlideConfirm.qml
    SettingSection.qml
    SettingRow.qml
    InfoText.qml
    InfoRow.qml
    ParamSegments.qml
    ParamSwitch.qml
    ParamNumber.qml
    ParamPick.qml
    ParamSlider.qml
    ParamText.qml
    ParamColor.qml
    SizePick.qml
    SegmentTabs.qml
    ColorSheet.qml
)
list(TRANSFORM DC_UI_QML PREPEND qml/ OUTPUT_VARIABLE DC_UI_QML_FILES)
# Served next to the module's qmldir (qrc:/DcUi/Main.qml), so that the Theme singleton resolves.
foreach(f IN LISTS DC_UI_QML)
    set_source_files_properties(qml/${f} PROPERTIES QT_RESOURCE_ALIAS ${f})
endforeach()
set_source_files_properties(qml/Theme.qml qml/Sheets.qml qml/Video.qml qml/Bindings.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(dc-ui
    URI DcUi
    VERSION 1.0
    RESOURCE_PREFIX /
    QML_FILES ${DC_UI_QML_FILES}
)

target_link_libraries(dc-ui PRIVATE dc::ui_core Qt6::Quick dc_version)
dc_apply_options(dc-ui)
if(NOT MSVC)
    # qmlcachegen output shadows its own parameters, and narrows JS lengths (qsizetype) to int
    target_compile_options(dc-ui PRIVATE -Wno-shadow -Wno-conversion)
endif()
