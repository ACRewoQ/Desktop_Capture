// capd: capture process. P0 skeleton only: it starts, reports its version and waits for Ctrl+C.
// Capture sources, compositor, encoder and muxer are built in P2 (lane B).

#include "dc/protocol/constants.hpp"
#include "dc/shmaudio/layout.hpp"
#include "dc/version.hpp"

#include <asio.hpp>

#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::cout << "capd " << dc::kVersionString << " (protocol " << dc::proto::kProtocolVersion
                      << ", shm audio v" << dc::shm::kVersion << ")\n";
            return 0;
        }
    }

    std::cout << "capd " << dc::kVersionString << " skeleton. Press Ctrl+C to exit.\n";
    asio::io_context io;
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const asio::error_code&, int) { io.stop(); });
    io.run();
    std::cout << "capd stopped\n";
    return 0;
}
