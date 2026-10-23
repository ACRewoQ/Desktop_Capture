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

# --- Opus (LAN media transport) ---
# vcpkg ships a CMake package (Opus::opus); distro packages have pkg-config only.
find_package(Opus CONFIG QUIET)
add_library(dc_opus INTERFACE)
if(TARGET Opus::opus)
    target_link_libraries(dc_opus INTERFACE Opus::opus)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DC_OPUS REQUIRED IMPORTED_TARGET opus)
    target_link_libraries(dc_opus INTERFACE PkgConfig::DC_OPUS)
endif()

# --- Catch2 (tests only) ---
if(DC_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()

# --- FFmpeg (capd, dc-capbench) ---
# Windows: vcpkg's FindFFMPEG wrapper sets FFMPEG_INCLUDE_DIRS / FFMPEG_LIBRARY_DIRS / FFMPEG_LIBRARIES.
# Linux: the distro's libav* packages through pkg-config, optional (capd's audio/MKV tests use them).
set(DC_HAVE_FFMPEG OFF)
if(WIN32)
    find_package(FFMPEG REQUIRED)
    add_library(dc_ffmpeg INTERFACE)
    target_include_directories(dc_ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(dc_ffmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(dc_ffmpeg INTERFACE ${FFMPEG_LIBRARIES})
    set(DC_HAVE_FFMPEG ON)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(DC_FFMPEG_PC QUIET IMPORTED_TARGET libavcodec libavformat libavutil)
    endif()
    if(DC_FFMPEG_PC_FOUND)
        add_library(dc_ffmpeg INTERFACE)
        target_link_libraries(dc_ffmpeg INTERFACE PkgConfig::DC_FFMPEG_PC)
        set(DC_HAVE_FFMPEG ON)
    else()
        message(STATUS "FFmpeg (libavcodec/libavformat/libavutil) not found: capd and its tests are skipped")
    endif()
endif()

# --- Qt 6 (UI and Qt based probes, optional) ---
# Prebuilt Qt from scripts/install-qt.ps1; DC_QT_DIR points at e.g. C:\dev\Qt\6.x.y\msvc2022_64.
option(DC_WITH_QT "Build Qt based targets when Qt 6 is found" ON)
set(DC_HAVE_QT OFF)
if(DC_WITH_QT)
    if(DEFINED ENV{DC_QT_DIR})
        list(APPEND CMAKE_PREFIX_PATH "$ENV{DC_QT_DIR}")
    endif()
    find_package(Qt6 6.4 QUIET COMPONENTS Quick)
    if(Qt6_FOUND)
        set(DC_HAVE_QT ON)
    else()
        message(STATUS "Qt 6 not found: Qt targets are skipped (set DC_QT_DIR, see scripts/install-qt.ps1)")
    endif()
endif()
