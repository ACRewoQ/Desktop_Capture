// Audio engine (mixd) inputs and outputs (P3 lane A2): process audio, devices that change their ID,
// device loss and recovery events. Fake devices and applications, so it runs on any platform.

#include "controller.hpp"
#include "device_manager.hpp"
#include "engine.hpp"
#include "fake_backend.hpp"
#include "plan_builder.hpp"

#include "dc/control/client.hpp"
#include "dc/protocol/defaults.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <thread>
#include <vector>

using namespace dc;
using namespace dc::mixd;
using namespace std::chrono_literals;
using Catch::Approx;

namespace {

proto::ParamTree defaultTree(int tracks = 4) {
    proto::ParamTree t;
    for (auto& c : proto::globalDefaults()) t.set(c.path, c.value);
    for (auto& c : proto::masterDefaults()) t.set(c.path, c.value);
    for (int n = 1; n <= tracks; ++n) {
        for (auto& c : proto::trackDefaults(n)) t.set(c.path, c.value);
        t.set("/track/" + std::to_string(n) + "/position", std::int64_t{n});
    }
    return t;
}

DeviceInfo dev(const std::string& id, const std::string& name, bool input, bool connected = true) {
    DeviceInfo d;
    d.id = id;
    d.name = name;
    d.input = input;
    d.channels = 2;
    d.connected = connected;
    return d;
}

FakeBackend::Process app(const std::string& exe, std::uint32_t pid, double tone) {
    FakeBackend::Process p;
    p.info = {exe, pid, exe + " window"};
    p.toneHz = tone;
    return p;
}

} // namespace

TEST_CASE("rematch: settings follow a device whose ID changed, by its name", "[mixd][a2]") {
    auto t = defaultTree(3);
    t.set("/track/1/input/kind", std::string("device"));
    t.set("/track/1/input/source", std::string("old-in"));
    t.set("/track/2/out/2/target", std::string("vmic/old-out"));
    t.set("/track/3/send/1/target", std::string("device/old-out"));
    t.set("/master/out/1/target", std::string("device/old-out"));
    t.set("/cue/target", std::string("device/twin-a"));
    t.set("/system/clock_device", std::string("old-out"));

    std::map<std::string, KnownDevice> known{{"old-in", {"UR22 In", true}},
                                             {"old-out", {"UR22 Out", false}},
                                             {"twin-a", {"USB Speaker", false}}};
    const std::vector<DeviceInfo> devices{dev("new-in", "UR22 In", true), dev("new-out", "UR22 Out", false),
                                          dev("twin-b", "USB Speaker", false), dev("twin-c", "USB Speaker", false)};

    auto r = rematchDevices(t, devices, known);
    REQUIRE(r.moved.size() == 2);
    std::map<std::string, std::string> changes;
    for (auto& c : r.changes) changes[c.path] = std::get<std::string>(c.value);
    CHECK(changes.at("/track/1/input/source") == "new-in");
    CHECK(changes.at("/track/2/out/2/target") == "vmic/new-out"); // the kind of target is kept
    CHECK(changes.at("/track/3/send/1/target") == "device/new-out");
    CHECK(changes.at("/master/out/1/target") == "device/new-out");
    CHECK(changes.at("/system/clock_device") == "new-out");
    // two devices carry that name: no guessing
    CHECK_FALSE(changes.count("/cue/target"));

    SECTION("a device that is still connected, or a same-named device already in use, is left alone") {
        auto t2 = defaultTree(2);
        t2.set("/master/out/1/target", std::string("device/old-out"));
        t2.set("/master/out/2/target", std::string("device/new-out"));
        CHECK(rematchDevices(t2, devices, known).changes.empty());
        auto still = devices;
        still.push_back(dev("old-out", "UR22 Out", false));
        auto t3 = defaultTree(1);
        t3.set("/master/out/1/target", std::string("device/old-out"));
        CHECK(rematchDevices(t3, still, known).changes.empty());
    }
    SECTION("a device Windows registered again with a number in its name (another USB port)") {
        CHECK(deviceBaseName("Line (2- Steinberg UR22)") == "Line (Steinberg UR22)");
        CHECK(deviceBaseName("12- USB Speaker") == "USB Speaker");
        CHECK(deviceBaseName("Mic (Model 2-X)") == "Mic (Model 2-X)");
        std::map<std::string, KnownDevice> k2{{"old-line", {"Line (Steinberg UR22)", true}}};
        auto t5 = defaultTree(1);
        t5.set("/track/1/input/kind", std::string("device"));
        t5.set("/track/1/input/source", std::string("old-line"));
        auto r5 = rematchDevices(t5, {dev("port2-line", "Line (2- Steinberg UR22)", true)}, k2);
        REQUIRE(r5.changes.size() == 1);
        CHECK(std::get<std::string>(r5.changes[0].value) == "port2-line");
        // an exact name wins over a numbered one
        auto r6 = rematchDevices(t5, {dev("port2-line", "Line (2- Steinberg UR22)", true), dev("same", "Line (Steinberg UR22)", true)}, k2);
        REQUIRE(r6.changes.size() == 1);
        CHECK(std::get<std::string>(r6.changes[0].value) == "same");
    }
    SECTION("inputs and outputs never match each other") {
        auto t4 = defaultTree(1);
        t4.set("/master/out/1/target", std::string("device/old-in"));
        CHECK(rematchDevices(t4, devices, known).changes.empty());
    }
}

TEST_CASE("process audio: one capture per application, lost when it exits, back when it restarts", "[mixd][a2]") {
    FakeBackend backend(FakeBackend::standardDevices(), {app("Game.exe", 100, 660)});
    Engine engine(nullptr);
    DeviceManager devices(backend, engine, {});
    std::mutex mutex;
    DeviceManager::State last;
    std::vector<DeviceManager::Event> events;
    devices.start([&](const DeviceManager::State& s) {
        std::lock_guard lock(mutex);
        last = s;
        events.insert(events.end(), s.events.begin(), s.events.end());
    });

    auto t = defaultTree(2);
    // two tracks on the same application, written differently
    t.set("/track/1/input/kind", std::string("process"));
    t.set("/track/1/input/source", std::string("game.exe"));
    t.set("/track/2/input/kind", std::string("process"));
    t.set("/track/2/input/source", std::string("GAME.EXE"));
    t.set("/track/2/mute", true);
    t.set("/master/out/1/target", std::string("device/fake-spk"));
    std::uint64_t gen = 0;
    auto state = [&] {
        std::lock_guard lock(mutex);
        return last;
    };
    auto settle = [&](auto pred) {
        for (int i = 0; i < 120; ++i) {
            devices.setDemand(deviceDemand(t));
            auto s = state();
            engine.setPlan(buildPlan(t, s.ports, ++gen).plan);
            engine.collectRetired();
            if (pred(s)) return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    };
    auto hasEvent = [&](const std::string& name) {
        std::lock_guard lock(mutex);
        return std::any_of(events.begin(), events.end(), [&](const DeviceManager::Event& e) {
            return e.name == name && e.id == "game.exe" && e.kind == "process";
        });
    };
    const auto key = processPortKey("game.exe");

    REQUIRE(settle([&](const DeviceManager::State& s) { return s.ports.inputs.count(key) == 1; }));
    CHECK(state().ports.inputs.size() == 1); // one capture for both tracks
    CHECK(state().processes.size() == 1);
    CHECK(buildPlan(t, state().ports, ++gen).inputState.at(2) == "ok");
    std::this_thread::sleep_for(1500ms); // the bridge waits 20 packets before it plays
    backend.takeOutputPeak("fake-spk");
    std::this_thread::sleep_for(400ms);
    CHECK(backend.takeOutputPeak("fake-spk") == Approx(0.25).margin(0.03));

    // the application exits: its tracks go "disconnected", device.lost
    backend.setProcessRunning("Game.exe", false);
    REQUIRE(settle([&](const DeviceManager::State& s) { return s.ports.inputs.count(key) == 0; }));
    CHECK(buildPlan(t, state().ports, ++gen).inputState.at(1) == "disconnected");
    CHECK(settle([&](const DeviceManager::State&) { return hasEvent("device.lost"); }));
    CHECK_FALSE(hasEvent("device.restored"));

    // it starts again under a new pid: reconnected within a few seconds, device.restored
    backend.setProcessRunning("Game.exe", true, 200);
    REQUIRE(settle([&](const DeviceManager::State& s) { return s.ports.inputs.count(key) == 1; }));
    CHECK(settle([&](const DeviceManager::State&) { return hasEvent("device.restored"); }));
    std::this_thread::sleep_for(1500ms);
    backend.takeOutputPeak("fake-spk");
    std::this_thread::sleep_for(400ms);
    CHECK(backend.takeOutputPeak("fake-spk") == Approx(0.25).margin(0.03));
    devices.stop();
}

TEST_CASE("mixd: device events, rematch by name and the process list behind the control server", "[mixd][a2][integration]") {
    asio::io_context io;
    proto::ParamStore store;
    Controller* ctlPtr = nullptr;
    control::ControlServer::Options opt;
    opt.port = 0;
    opt.onDelta = [&](const proto::Delta& d) {
        if (ctlPtr) ctlPtr->onDelta(d);
    };
    control::ControlServer server(io, store, opt);
    FakeBackend backend(FakeBackend::standardDevices(), {app("Player.exe", 42, 440)});
    Engine engine(nullptr);
    DeviceManager devices(backend, engine, {});
    Controller::Options co;
    co.verbose = false;
    Controller ctl(io, server, devices, engine, nullptr, co);
    server.start();
    ctl.start(devices.enumerateNow());
    ctlPtr = &ctl;
    devices.start([&](const DeviceManager::State& s) { ctl.onDeviceState(s); });
    auto work = asio::make_work_guard(io);
    std::thread net([&] { io.run(); });

    control::ControlClient::Options o;
    o.port = server.port();
    o.clientName = "test";
    control::ControlClient client(o);
    std::mutex mutex;
    std::vector<proto::Event> events;
    client.onEvent = [&](const proto::Event& e) {
        std::lock_guard lock(mutex);
        events.push_back(e);
    };
    REQUIRE(client.connect(3s));
    auto waitFor = [&](auto pred) {
        for (int i = 0; i < 160; ++i) {
            if (pred(client.tree())) return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    };
    auto event = [&](const std::string& name, const std::string& key, const std::string& value) {
        for (int i = 0; i < 160; ++i) {
            {
                std::lock_guard lock(mutex);
                for (const auto& e : events)
                    if (e.name == name)
                        if (auto it = e.data.find(key); it != e.data.end() && std::get<std::string>(it->second) == value)
                            return true;
            }
            std::this_thread::sleep_for(50ms);
        }
        return false;
    };

    // the application list
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/devices/process/1/exe") == "Player.exe"; }));
    CHECK(client.tree().getAs<std::int64_t>("/devices/process/1/pid") == 42);

    REQUIRE(client.set({{"/track/1/input/kind", std::string("device")}, {"/track/1/input/source", std::string("fake-line")},
                        {"/track/2/input/kind", std::string("process")}, {"/track/2/input/source", std::string("player.exe")}})
                .get()
                .code == proto::ErrorCode::Ok);
    CHECK(waitFor([](const proto::ParamTree& t) {
        return t.getAs<std::string>("/track/1/input/state") == "ok" && t.getAs<std::string>("/track/2/input/state") == "ok";
    }));

    // unplugged and back: device.lost, device.restored, the track follows
    backend.setConnected("fake-line", false);
    CHECK(event("device.lost", "id", "fake-line"));
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/track/1/input/state") == "disconnected"; }));
    backend.setConnected("fake-line", true);
    CHECK(event("device.restored", "id", "fake-line"));
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/track/1/input/state") == "ok"; }));

    // Windows gives the line input and the speakers new IDs: the settings move with them
    backend.changeId("fake-line", "fake-line-2");
    backend.changeId("fake-spk", "fake-spk-2");
    CHECK(event("device.rematched", "id", "fake-line-2"));
    CHECK(waitFor([](const proto::ParamTree& t) {
        return t.getAs<std::string>("/track/1/input/source") == "fake-line-2" &&
               t.getAs<std::string>("/master/out/1/target") == "device/fake-spk-2" &&
               t.getAs<std::string>("/track/1/input/state") == "ok" &&
               t.getAs<std::string>("/system/clock_active") == "device/fake-spk-2";
    }));

    client.disconnect();
    ctl.stop();
    server.stop();
    work.reset();
    asio::post(io, [&] { io.stop(); });
    net.join();
    ctlPtr = nullptr;
    devices.stop();
}
