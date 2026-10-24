// mixd: audio engine process. Owns the parameter tree (control server on TCP 47800), mixes up to 64
// tracks from WASAPI input devices to output devices, sends meters, and writes the tracks the capture
// output records to shared memory for capd.
//
//   mixd [--port N] [--bind ADDR] [--allow-remote] [--fake-devices] [--shm-suffix S | --no-shm]
//        [--bridge-ms MS] [--test-tone HZ] [--quiet] [--list-devices] [--version]

#include "src/controller.hpp"
#include "src/device_manager.hpp"
#include "src/engine.hpp"
#include "src/fake_backend.hpp"

#include "dc/protocol/constants.hpp"
#include "dc/shmaudio/layout.hpp"
#include "dc/shmaudio/shared_memory.hpp"
#include "dc/version.hpp"

#include <asio.hpp>

#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

#ifdef _WIN32
#include <windows.h>
static std::uint32_t currentPid() { return GetCurrentProcessId(); }
#else
#include <unistd.h>
static std::uint32_t currentPid() { return static_cast<std::uint32_t>(getpid()); }
#endif

namespace {

int usage() {
    std::cerr << "usage: mixd [--port N] [--bind ADDR] [--allow-remote] [--fake-devices] [--shm-suffix S | --no-shm]\n"
                 "            [--bridge-ms MS] [--test-tone HZ] [--quiet] [--list-devices] [--version]\n"
                 "  --fake-devices  in-process test devices instead of WASAPI (always on outside Windows)\n"
                 "  --bridge-ms     minimum latency of the clock bridge to non-clock devices (default: automatic)\n"
                 "  --test-tone     add a -20 dBFS sine of HZ to the master (dropout tests)\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    using namespace dc;
    control::ControlServer::Options opt;
    opt.port = proto::kDefaultControlPort;
    opt.serverName = "mixd";
    opt.serverId = "mixd";
    opt.sessionId = std::random_device{}();
    mixd::DeviceManager::Options devOpt;
    mixd::Controller::Options ctlOpt;
    bool fake = false, useShm = true, quiet = false, list = false;
    std::string shmSuffix;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << a << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--version") {
            std::cout << "mixd " << kVersionString << " (protocol " << proto::kProtocolVersion << ")\n";
            return 0;
        } else if (a == "--port") opt.port = static_cast<std::uint16_t>(std::stoi(next()));
        else if (a == "--bind") opt.bindAddress = next();
        else if (a == "--allow-remote") opt.allowRemote = true;
        else if (a == "--fake-devices") fake = true;
        else if (a == "--no-shm") useShm = false;
        else if (a == "--shm-suffix") shmSuffix = next();
        else if (a == "--bridge-ms") devOpt.bridgeMinMs = std::stod(next());
        else if (a == "--test-tone") {
            ctlOpt.build.testToneHz = std::stod(next());
            ctlOpt.build.testToneGain = 0.1f;
        } else if (a == "--quiet") quiet = true;
        else if (a == "--list-devices") list = true;
        else return usage();
    }
    if (opt.allowRemote && opt.bindAddress == "127.0.0.1") opt.bindAddress = "0.0.0.0";
    ctlOpt.verbose = !quiet;

    try {
        std::unique_ptr<mixd::DeviceBackend> backend;
#ifdef _WIN32
        if (!fake) backend = mixd::makeWasapiBackend();
#endif
        if (!backend) backend = std::make_unique<mixd::FakeBackend>();

        if (list) {
            for (const auto& d : backend->enumerate())
                std::cout << (d.input ? "in  " : "out ") << (d.connected ? "ok   " : "gone ") << (d.isDefault ? "* " : "  ")
                          << d.channels << "ch  " << d.name << "  [" << d.id << "]\n";
            return 0;
        }

        shm::SharedMemory memory;
        std::unique_ptr<shm::AudioRingWriter> writer;
        if (useShm) {
            std::string err;
            memory = shm::SharedMemory::create(shm::kLayout.totalBytes, shmSuffix, &err);
            if (!memory.valid()) {
                std::cerr << "shared memory: " << err << " (continuing without it)\n";
            } else {
                writer = std::make_unique<shm::AudioRingWriter>(memory.data(), currentPid());
            }
        }

        asio::io_context io;
        proto::ParamStore store;
        mixd::Controller* controller = nullptr;
        opt.onDelta = [&controller](const proto::Delta& d) {
            if (controller) controller->onDelta(d);
        };
        opt.onClient = [&controller](const control::ClientInfo& c, bool connected) {
            if (controller) controller->onClient(c, connected);
        };
        if (!quiet) {
            opt.log = [](const control::ClientInfo& c, const std::string& text) {
                std::cout << "[" << c.name << "@" << c.address << "] " << text << std::endl;
            };
        }
        control::ControlServer server(io, store, opt);
        mixd::Engine engine(writer.get());
        mixd::DeviceManager devices(*backend, engine, devOpt);
        mixd::Controller ctl(io, server, devices, engine, writer.get(), ctlOpt);

        server.start();
        ctl.start(devices.enumerateNow());
        controller = &ctl;
        devices.start([&ctl](const mixd::DeviceManager::State& s) { ctl.onDeviceState(s); });
        std::cout << "mixd " << kVersionString << " listening on " << opt.bindAddress << ":" << server.port()
                  << (fake ? " (fake devices)" : "") << (writer ? ", shared memory on" : "") << ". Ctrl+C to stop."
                  << std::endl;

        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](const asio::error_code&, int) {
            ctl.stop();
            server.stop();
            asio::post(io, [&] { io.stop(); });
        });
        io.run();
        controller = nullptr;
        devices.stop(); // stops every stream before the engine goes away
        std::cout << "mixd stopped" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
