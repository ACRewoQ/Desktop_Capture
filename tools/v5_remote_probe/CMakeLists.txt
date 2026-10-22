# P1/V5: footprint and multi-touch probe for the remote controller (Qt 6 Quick).
# Built only when Qt 6 is found (see cmake/DcDependencies.cmake).

find_package(Qt6 REQUIRED COMPONENTS Quick Network)

qt_add_executable(dc-v5-probe
    main.cpp
    probe.cpp
    probe.hpp
    strip_model.cpp
    strip_model.hpp
    touch_selftest.cpp
    touch_selftest.hpp
)
set_target_properties(dc-v5-probe PROPERTIES AUTOMOC ON WIN32_EXECUTABLE TRUE)

qt_add_qml_module(dc-v5-probe
    URI DcProbe
    VERSION 1.0
    RESOURCE_PREFIX /
    QML_FILES
        qml/Main.qml
        qml/MainControls.qml
        qml/Minimal.qml
        qml/Strip.qml
        qml/Fader.qml
        qml/Meter.qml
        qml/StatsBar.qml
)

target_link_libraries(dc-v5-probe PRIVATE Qt6::Quick Qt6::Network dc::control dc_version)
if(WIN32)
    target_link_libraries(dc-v5-probe PRIVATE user32)
else()
    # Synthetic touch for the self test off Windows (development only; Windows injects real touch input)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    target_link_libraries(dc-v5-probe PRIVATE Qt6::Test)
endif()
dc_apply_options(dc-v5-probe)
if(NOT MSVC)
    # qmlcachegen output shadows its own parameters
    target_compile_options(dc-v5-probe PRIVATE -Wno-shadow)
endif()
