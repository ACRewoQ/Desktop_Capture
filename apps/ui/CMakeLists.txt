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
    src/connection.cpp
    src/connection.hpp
    src/exercise.cpp
    src/exercise.hpp
    src/param_hub.cpp
    src/param_hub.hpp
    src/recorder.cpp
    src/recorder.hpp
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
target_link_libraries(dc_ui_core PUBLIC Qt6::Core Qt6::Network Qt6::Qml dc::protocol)
dc_apply_options(dc_ui_core)

qt_add_executable(dc-ui src/main.cpp)
set_target_properties(dc-ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE TRUE)

set(DC_UI_QML
    Main.qml
    Theme.qml
    Sheets.qml
    TopBar.qml
    NavRail.qml
    RecordButton.qml
    # pages
    MixerPage.qml
    SettingsPage.qml
    VideoPage.qml
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
    LayerCanvas.qml
    LayerSheet.qml
    SourceSheet.qml
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
set_source_files_properties(qml/Theme.qml qml/Sheets.qml qml/Video.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
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
