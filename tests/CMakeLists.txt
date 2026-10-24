add_executable(dc_tests
    test_clocksync.cpp
    test_codec.cpp
    test_integration.cpp
    test_lanmedia.cpp
    test_meter.cpp
    test_mixd.cpp
    test_param_store.cpp
    test_param_tree.cpp
    test_routing.cpp
    test_schema.cpp
    test_shm.cpp
    test_video.cpp
)
target_link_libraries(dc_tests PRIVATE dc::protocol dc::control dc::shmaudio dc::clocksync dc::lanmedia dc::mock_engine dc::mixd_core Catch2::Catch2WithMain)
dc_apply_options(dc_tests)
if(TARGET capd_core)
    target_sources(dc_tests PRIVATE test_capd.cpp test_capd_video.cpp)
    target_link_libraries(dc_tests PRIVATE capd_core)
endif()

include(Catch)
catch_discover_tests(dc_tests)

if(DC_BUILD_TOOLS)
    add_test(NAME schema_doc_up_to_date
        COMMAND ${CMAKE_COMMAND} -DDC_CLI=$<TARGET_FILE:dc-cli> -DDOC=${PROJECT_SOURCE_DIR}/docs/spec/protocol-params.md
                -P ${PROJECT_SOURCE_DIR}/cmake/CheckSchemaDoc.cmake)
endif()

if(DC_HAVE_QT AND TARGET dc::ui_core)
    add_subdirectory(ui)
endif()
