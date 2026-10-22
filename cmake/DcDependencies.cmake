# Third-party dependencies. On Windows they come from vcpkg (vcpkg.json);
# on Linux developer machines the distro packages work as well.

find_package(Threads REQUIRED)

# --- asio (standalone, header-only) ---
find_package(asio CONFIG QUIET)
if(NOT TARGET asio::asio)
    find_path(DC_ASIO_INCLUDE_DIR asio.hpp REQUIRED)
    add_library(asio::asio INTERFACE IMPORTED)
    set_target_properties(asio::asio PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${DC_ASIO_INCLUDE_DIR}")
endif()
add_library(dc_asio INTERFACE)
target_link_libraries(dc_asio INTERFACE asio::asio Threads::Threads)
target_compile_definitions(dc_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
if(WIN32)
    target_link_libraries(dc_asio INTERFACE ws2_32 mswsock)
endif()

# --- msgpack-cxx (header-only) ---
find_package(msgpack-cxx CONFIG REQUIRED)
add_library(dc_msgpack INTERFACE)
target_link_libraries(dc_msgpack INTERFACE msgpack-cxx)
if(WIN32)
    # vcpkg builds msgpack without Boost; make sure the headers agree.
    target_compile_definitions(dc_msgpack INTERFACE MSGPACK_NO_BOOST)
endif()

# --- Catch2 (tests only) ---
if(DC_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()
