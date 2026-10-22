// dc-mock-server: fake engine for UI development and protocol tests.
//   dc-mock-server [--port N] [--bind ADDR] [--allow-remote] [--no-meters] [--quiet]

#include "mock_engine.hpp"

#include "dc/protocol/constants.hpp"
#include "dc/version.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>

int main(int argc, char** argv) {
    dc::control::ControlServer::Options opt;
    opt.port = dc::proto::kDefaultControlPort;
    opt.serverName = "dc-mock-server";
    opt.serverId = "mock";
    opt.sessionId = std::random_device{}();
    dc::mock::MockEngine::Options mockOpt;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << a << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--port") opt.port = static_cast<std::uint16_t>(std::stoi(next()));
        else if (a == "--bind") opt.bindAddress = next();
        else if (a == "--allow-remote") opt.allowRemote = true;
        else if (a == "--no-meters") mockOpt.meters = false;
        else if (a == "--quiet") quiet = true;
        else if (a == "--version") {
            std::cout << "dc-mock-server " << dc::kVersionString << " (protocol " << dc::proto::kProtocolVersion
                      << ")\n";
            return 0;
        } else {
            std::cerr << "usage: dc-mock-server [--port N] [--bind ADDR] [--allow-remote] [--no-meters] [--quiet]\n";
            return 2;
        }
    }
    if (opt.allowRemote && opt.bindAddress == "127.0.0.1") opt.bindAddress = "0.0.0.0";
    if (!quiet) {
        opt.log = [](const dc::control::ClientInfo& c, const std::string& text) {
            std::cout << "[" << c.name << "@" << c.address << "] " << text << std::endl;
        };
    }

    try {
        asio::io_context io;
        dc::proto::ParamStore store;
        dc::control::ControlServer server(io, store, opt);
        dc::mock::MockEngine engine(io, server, mockOpt);
        server.start();
        engine.start();
        std::cout << "dc-mock-server listening on " << opt.bindAddress << ":" << server.port() << " ("
                  << store.tree().size() << " parameters). Ctrl+C to stop." << std::endl;

        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](const asio::error_code&, int) {
            engine.stop();
            server.stop();
            asio::post(io, [&] { io.stop(); });
        });
        io.run();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
