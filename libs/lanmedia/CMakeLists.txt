add_library(dc_lanmedia
    src/arrival_clock.cpp
    src/audio_codec.cpp
    src/audio_receiver.cpp
    src/audio_sender.cpp
    src/net_sim.cpp
    src/packet.cpp
    src/udp_transport.cpp
    src/video_receiver.cpp
    src/video_sender.cpp
    src/video_sim.cpp
)
add_library(dc::lanmedia ALIAS dc_lanmedia)
target_include_directories(dc_lanmedia PUBLIC include)
target_link_libraries(dc_lanmedia PUBLIC dc::clocksync dc_asio PRIVATE dc::shmaudio dc_opus)
dc_apply_options(dc_lanmedia)
