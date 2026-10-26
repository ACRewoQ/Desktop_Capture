add_library(dc_protocol
    src/codec.cpp
    src/control.cpp
    src/defaults.cpp
    src/fx.cpp
    src/lan.cpp
    src/meter.cpp
    src/names.cpp
    src/param_store.cpp
    src/param_tree.cpp
    src/routing.cpp
    src/schema.cpp
    src/schema_v1.cpp
    src/video.cpp
)
add_library(dc::protocol ALIAS dc_protocol)
target_include_directories(dc_protocol PUBLIC include)
target_link_libraries(dc_protocol PRIVATE dc_msgpack)
dc_apply_options(dc_protocol)
