# Qt side of the UI (protocol client, models, self test) against the in-process mock engine.
find_package(Qt6 REQUIRED COMPONENTS Test)
add_executable(dc_ui_tests test_ui.cpp)
set_target_properties(dc_ui_tests PROPERTIES AUTOMOC ON)
target_link_libraries(dc_ui_tests PRIVATE dc::ui_core dc::control dc::mock_engine Qt6::Test)
dc_apply_options(dc_ui_tests)
add_test(NAME ui_tests COMMAND dc_ui_tests)
if(WIN32)
    # A repository build has no Qt DLLs next to it.
    set_tests_properties(ui_tests PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
