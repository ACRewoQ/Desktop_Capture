add_library(dc_control
    src/client.cpp
    src/server.cpp
    src/video_methods.cpp
)
add_library(dc::control ALIAS dc_control)
target_include_directories(dc_control PUBLIC include)
target_link_libraries(dc_control PUBLIC dc::protocol dc_asio PRIVATE dc::shmaudio)
dc_apply_options(dc_control)
