add_executable(dc-shm-tone main.cpp)
target_link_libraries(dc-shm-tone PRIVATE dc::shmaudio)
dc_apply_options(dc-shm-tone)
