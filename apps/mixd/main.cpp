// mixd: audio engine process. Owns the parameter tree (control server on TCP 47800), mixes up to 64
// tracks from WASAPI input devices to output devices, sends meters, and writes the tracks the capture
// output records to shared memory for capd. On the LAN it serves paired remote controllers, sends and
// receives audio streams (UDP 47810) and finds other engines by mDNS (docs/spec/protocol.md 8, 10).
//
//   mixd [--port N] [--bind ADDR] [--local-only] [--allow-remote] [--fake-devices] [--shm-suffix S | --no-shm]
//        [--media-port N] [--lan-store FILE] [--no-discovery] [--control-store FILE] [--no-hotkeys]
//        [--profile-dir DIR] [--profile NAME] [--no-profiles]
//        [--bridge-ms MS] [--test-tone HZ] [--test-pulse SEC] [--no-asio] [--plugin-cache FILE] [--no-plugins]
//        [--stop-event NAME] [--quiet] [--list-devices] [--version]
//   mixd --scan-plugin FILE --out JSON      (the VST3 scanner child process, started by mixd itself)
//
// With VST3 (DC_WITH_JUCE) the main thread runs JUCE's message loop (plugin creation and windows)
// and the engine runs on a second thread.

#include "src/control_bindings.hpp"
#include "src/controller.hpp"
#include "src/device_manager.hpp"
#include "src/engine.hpp"
#include "src/fake_backend.hpp"
#include "src/lan_service.hpp"
#include "src/profile_manager.hpp"
#ifdef DC_HAVE_VST3
#include "vst3_host.hpp"
#endif

#include "dc/control/stop_event.hpp"
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

#ifdef _WIN32
constexpr bool kRealDevices = true;
#else
constexpr bool kRealDevices = false; // fake devices only
#endif

int usage() {
    std::cerr << "usage: mixd [--port N] [--bind ADDR] [--local-only] [--allow-remote] [--fake-devices]\n"
                 "            [--shm-suffix S | --no-shm] [--media-port N] [--lan-store FILE] [--no-discovery]\n"
                 "            [--bridge-ms MS] [--test-tone HZ] [--test-pulse SEC] [--no-asio]\n"
                 "            [--plugin-cache FILE] [--no-plugins] [--control-store FILE] [--no-hotkeys]\n"
                 "            [--profile-dir DIR] [--profile NAME] [--no-profiles]\n"
                 "            [--stop-event NAME] [--quiet] [--list-devices] [--version]\n"
                 "  --bind          control address (default 0.0.0.0: LAN clients must pair first)\n"
                 "  --local-only    control on 127.0.0.1 only, no LAN streams or discovery\n"
                 "  --allow-remote  accept LAN clients without pairing (development only)\n"
                 "  --media-port    UDP port of the LAN audio streams (default 47810)\n"
                 "  --lan-store     file with the server id and pairings (default %APPDATA%\\DesktopCapture\\mixd-lan.txt)\n"
                 "  --fake-devices  in-process test devices instead of WASAPI (always on outside Windows)\n"
                 "  --bridge-ms     minimum latency of the clock bridge to non-clock devices (default: automatic)\n"
                 "  --test-tone     add a -20 dBFS sine of HZ to the master (dropout tests)\n"
                 "  --test-pulse    make the tone (1 kHz unless --test-tone) a 50 ms burst at every multiple of\n"
                 "                  SEC seconds of QPC time (A/V sync check with dc-av-flash)\n"
                 "  --no-asio       do not list or open ASIO drivers\n"
                 "  --plugin-cache  VST3 scan results file (default %APPDATA%\\DesktopCapture\\vst3-cache.json)\n"
                 "  --no-plugins    no VST3 host (built-in effects only)\n"
                 "  --control-store hotkey and MIDI bindings file (default %APPDATA%\\DesktopCapture\\mixd-control.txt)\n"
                 "  --no-hotkeys    no hotkeys and no MIDI input (tests running next to a real engine)\n"
                 "  --stop-event    stop cleanly when this named event is set (dc-tray, which has no console for Ctrl+C)\n"
                 "  --profile-dir   profile folder (default %APPDATA%\\DesktopCapture\\profiles; the current profile is\n"
                 "                  remembered in DIR\\mixd-profile.txt). With --fake-devices profiles are off unless given\n"
                 "  --profile       start with this profile instead of the last one (a new name starts fresh)\n"
                 "  --no-profiles   do not read or write profiles (snapshots stay in memory)\n"
                 "  --list-devices  print the devices and the applications with audio, then exit\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    using namespace dc;
    control::ControlServer::Options opt;
    opt.port = proto::kDefaultControlPort;
    opt.serverName = "mixd";
    opt.bindAddress = "0.0.0.0";
    opt.sessionId = std::random_device{}();
    mixd::DeviceManager::Options devOpt;
    mixd::Controller::Options ctlOpt;
    mixd::LanService::Options lanOpt;
    lanOpt.storePath = mixd::LanStore::defaultPath();
    bool fake = false, useShm = true, quiet = false, list = false, localOnly = false, asioDrivers = true, plugins = true;
    std::string shmSuffix, pluginCache, scanFile, scanOut, stopEvent;
    mixd::ControlBindings::Options bindOpt;
    bindOpt.storePath = mixd::ControlBindings::defaultPath();
    bool hotkeys = true;
    mixd::ProfileManager::Options profOpt;
    std::string profileDir;
    bool profiles = true;

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
        else if (a == "--local-only") localOnly = true;
        else if (a == "--media-port") lanOpt.mediaPort = static_cast<std::uint16_t>(std::stoi(next()));
        else if (a == "--lan-store") lanOpt.storePath = next();
        else if (a == "--no-discovery") lanOpt.discovery = false;
        else if (a == "--fake-devices") fake = true;
        else if (a == "--no-shm") useShm = false;
        else if (a == "--shm-suffix") shmSuffix = next();
        else if (a == "--bridge-ms") devOpt.bridgeMinMs = std::stod(next());
        else if (a == "--test-tone") {
            ctlOpt.build.testToneHz = std::stod(next());
            ctlOpt.build.testToneGain = 0.1f;
        } else if (a == "--test-pulse") {
            ctlOpt.build.testPulseSeconds = std::stod(next());
            ctlOpt.build.testToneGain = 0.1f;
        } else if (a == "--quiet") quiet = true;
        else if (a == "--plugin-cache") pluginCache = next();
        else if (a == "--no-plugins") plugins = false;
        else if (a == "--control-store") bindOpt.storePath = next();
        else if (a == "--no-hotkeys") hotkeys = false;
        else if (a == "--stop-event") stopEvent = next();
        else if (a == "--profile-dir") profileDir = next();
        else if (a == "--profile") profOpt.startProfile = next();
        else if (a == "--no-profiles") profiles = false;
        else if (a == "--scan-plugin") scanFile = next();
        else if (a == "--out") scanOut = next();
        else if (a == "--list-devices") list = true;
        else if (a == "--no-asio") asioDrivers = false;
        else return usage();
    }
    if (!scanFile.empty()) {
#ifdef DC_HAVE_VST3
        return mixd::vst3::scanChildMain(scanFile, scanOut);
#else
        std::cerr << "this mixd was built without VST3 support\n";
        return 2;
#endif
    }
    if (localOnly) opt.bindAddress = "127.0.0.1";
    ctlOpt.verbose = !quiet;
    lanOpt.verbose = !quiet;
    bindOpt.verbose = !quiet;
    profOpt.verbose = !quiet;
    if (profiles && !profileDir.empty()) {
        profOpt.directory = profileDir;
        profOpt.statePath = profileDir + "/mixd-profile.txt";
    } else if (profiles && !fake && kRealDevices) {
        // test runs with fake devices never touch the real profiles
        profOpt.directory = mixd::ProfileManager::defaultDirectory();
        profOpt.statePath = mixd::ProfileManager::defaultStatePath();
    }

    auto runEngine = [&](mixd::PluginHost* host) -> int {
        ctlOpt.plugins = host;
        try {
            std::unique_ptr<mixd::DeviceBackend> backend;
#ifdef _WIN32
            if (!fake) backend = mixd::makeWasapiBackend(asioDrivers);
#else
            (void)asioDrivers;
#endif
            if (!backend) backend = std::make_unique<mixd::FakeBackend>();

            if (list) {
                for (const auto& d : backend->enumerate())
                    std::cout << (d.input ? "in  " : "out ") << (d.connected ? "ok   " : "gone ") << (d.isDefault ? "* " : "  ")
                              << d.channels << "ch " << (d.sampleRate ? std::to_string(d.sampleRate) + "Hz  " : std::string("  "))
                              << (d.asio ? "asio " : d.virtualCable ? "vmic " : "") << d.name << "  [" << d.id << "]\n";
                for (const auto& p : backend->enumerateProcesses())
                    std::cout << "app " << p.exe << "  pid " << p.pid << (p.title.empty() ? "" : "  \"" + p.title + "\"") << "\n";
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
            std::unique_ptr<mixd::LanService> lan;
            if (!localOnly) {
                lan = std::make_unique<mixd::LanService>(io, lanOpt);
                opt.serverId = lan->serverId();
            opt.serverName = mixd::computerName(); // what peers and remote controllers show for this PC
                opt.authorize = [&lan](const proto::Hello& h, const control::ClientInfo& c) { return lan->authorize(h, c); };
                ctlOpt.lan = lan.get();
            } else {
                opt.serverId = "mixd";
            }
            mixd::Controller* controller = nullptr;
            mixd::ControlBindings* bindings = nullptr;
            mixd::ProfileManager* profileManager = nullptr;
            opt.onDelta = [&controller, &bindings, &profileManager](const proto::Delta& d) {
                if (controller) controller->onDelta(d);
                if (bindings) bindings->onDelta(d);
                if (profileManager) profileManager->onDelta(d);
            };
            ctlOpt.onDevicesRefresh = [&bindings, &profileManager] {
                if (bindings) bindings->refreshMidi();
                if (profileManager) profileManager->refreshList();
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
            std::unique_ptr<mixd::HotkeyBackend> keyInput;
            std::unique_ptr<mixd::MidiBackend> midiInput;
            if (hotkeys) {
                keyInput = mixd::makeHotkeyBackend();
                midiInput = mixd::makeMidiBackend();
            } else {
                keyInput = mixd::makeDisabledHotkeys();
                midiInput = mixd::makeDisabledMidi();
            }
            mixd::ControlBindings bind(io, server, *keyInput, *midiInput, bindOpt);
            mixd::ProfileManager::Hooks hooks;
            hooks.restoreFx = [&ctl](const auto& slots, const auto& states) { return ctl.fx().restore(slots, states); };
            hooks.restored = [&ctl] { ctl.fx().restored(); };
            hooks.pluginStates = [&ctl] { return ctl.fx().pluginStates(); };
            hooks.deviceNames = [&ctl](const std::vector<std::string>& ids) { return ctl.deviceNames(ids); };
            hooks.rememberDevices = [&ctl](const std::vector<proto::SavedDevice>& d) { ctl.rememberDevices(d); };
            mixd::ProfileManager prof(io, server, std::move(hooks), profOpt);

            server.start();
            ctl.start(devices.enumerateNow());
            if (lan) lan->start(server, server.port(), [&ctl] { ctl.portsChanged(); });
            controller = &ctl;
            bind.start();
            bindings = &bind;
            prof.start(); // the last profile replaces the seeded tree before any device opens
            profileManager = &prof;
            devices.start([&ctl](const mixd::DeviceManager::State& s) { ctl.onDeviceState(s); });
            std::cout << "mixd " << kVersionString << " listening on " << opt.bindAddress << ":" << server.port()
                      << (fake ? " (fake devices)" : "") << (writer ? ", shared memory on" : "")
                      << (host ? ", VST3 on" : "")
                      << (profOpt.directory.empty() ? ", profiles off" : ", profile " + server.store().tree().getAs<std::string>("/profile/current").value_or(""))
                      << ". Ctrl+C to stop."
                      << std::endl;

            bool stopping = false;
            auto stopEngine = [&] {
                if (stopping) return;
                stopping = true;
                prof.stop(); // the last changes, with the plugins' states
                profileManager = nullptr;
                if (lan) lan->stop();
                ctl.stop();
                bindings = nullptr;
                bind.stop();
                server.stop();
                asio::post(io, [&] { io.stop(); });
            };
            asio::signal_set signals(io, SIGINT, SIGTERM);
            signals.async_wait([&](const asio::error_code& ec, int) {
                if (!ec) stopEngine();
            });
            control::StopEventWatcher stopWatch(stopEvent, [&] {
                if (!quiet) std::cout << "stop requested" << std::endl;
                asio::post(io, stopEngine);
            });
            io.run();
            controller = nullptr;
            bindings = nullptr;
            profileManager = nullptr;
            devices.stop(); // stops every stream before the engine goes away
            lan.reset();
            std::cout << "mixd stopped" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 1;
        }
        return 0;
    };

#ifdef DC_HAVE_VST3
    if (plugins && !list) {
        return mixd::vst3::runWithMessageLoop([&] {
            mixd::vst3::HostOptions ho;
            ho.cacheFile = pluginCache;
            ho.verbose = !quiet;
            auto host = mixd::vst3::makeHost(ho);
            return runEngine(host.get());
        });
    }
#else
    (void)plugins;
#endif
    return runEngine(nullptr);
}
