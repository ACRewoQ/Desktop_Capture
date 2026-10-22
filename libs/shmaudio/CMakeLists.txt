add_library(dc_shmaudio
    src/clock.cpp
    src/ring.cpp
    src/shared_memory.cpp
)
add_library(dc::shmaudio ALIAS dc_shmaudio)
target_include_directories(dc_shmaudio PUBLIC include)
if(UNIX AND NOT APPLE)
    target_link_libraries(dc_shmaudio PUBLIC rt)
endif()
dc_apply_options(dc_shmaudio)
