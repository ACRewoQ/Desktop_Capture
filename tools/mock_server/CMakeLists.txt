add_library(dc_mock_engine mock_engine.cpp)
add_library(dc::mock_engine ALIAS dc_mock_engine)
target_include_directories(dc_mock_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dc_mock_engine PUBLIC dc::control PRIVATE dc_version)
dc_apply_options(dc_mock_engine)

add_executable(dc-mock-server main.cpp)
target_link_libraries(dc-mock-server PRIVATE dc::mock_engine dc_version)
dc_apply_options(dc-mock-server)
