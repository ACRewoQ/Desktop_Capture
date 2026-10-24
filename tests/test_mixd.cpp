// Audio engine (mixd): DSP parts, the mixer, plan building from the parameter tree, and the whole
// engine with in-process fake devices behind the real control server.

#include "controller.hpp"
#include "device_manager.hpp"
#include "dsp.hpp"
#include "engine.hpp"
#include "fake_backend.hpp"
#include "mixer.hpp"
#include "plan_builder.hpp"

#include "dc/control/client.hpp"
#include "dc/protocol/defaults.hpp"
#include "dc/shmaudio/layout.hpp"
#include "dc/shmaudio/ring.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <future>
#include <thread>
#include <vector>

using namespace dc;
using namespace dc::mixd;
using namespace std::chrono_literals;
using Catch::Approx;

namespace {

// steady-state peak of a filter for a sine of freq
float filteredPeak(Biquad::Type type, double cutoff, double freq) {
    StripFilter f(type);
    f.reset(true, cutoff);
    std::vector<float> buf(4800);
    double phase = 0;
    float peak = 0;
    for (int block = 0; block < 20; ++block) {
        for (auto& v : buf) {
            v = static_cast<float>(std::sin(phase));
            phase += 2 * kPi * freq / kSampleRate;
        }
        float* ch[1] = {buf.data()};
        f.process(ch, 1, static_cast<int>(buf.size()), true, cutoff);
        if (block >= 10)
            for (float v : buf) peak = std::max(peak, std::fabs(v));
    }
    return peak;
}

std::shared_ptr<InputPort> port(const std::string& id, int channels) {
    return std::make_shared<InputPort>(id, channels, clocksync::ClockBridge::Config{});
}
std::shared_ptr<OutputPort> outPort(const std::string& id) {
    return std::make_shared<OutputPort>(id, clocksync::ClockBridge::Config{});
}

// Runs the mixer long enough for smoothing to settle; returns the peak of each bus channel in the
// last block. input = constant DC level on every channel of every input port.
std::vector<std::array<float, 2>> runMixer(Mixer& m, const MixPlan& plan, float input, int blocks = 200) {
    std::vector<std::vector<float>> in;
    std::vector<const float*> ptrs;
    for (const auto& p : plan.inputs) in.emplace_back(static_cast<std::size_t>(256 * p->channels), input);
    for (auto& v : in) ptrs.push_back(v.data());
    std::vector<std::array<float, 2>> out(plan.buses.size());
    for (int b = 0; b < blocks; ++b) m.process(plan, 256, ptrs.data(), nullptr);
    for (std::size_t b = 0; b < plan.buses.size(); ++b)
        for (int c = 0; c < 2; ++c) out[b][c] = m.bus(static_cast<int>(b), c)[255];
    return out;
}

StripPlan track(int id, int channels, int input) {
    StripPlan s;
    s.id = id;
    s.channels = channels;
    s.input = input;
    return s;
}

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

} // namespace

TEST_CASE("Lo-Hi filters cut below and above their frequency", "[mixd]") {
    // 2nd order: -40 dB one decade away, untouched one decade inside
    CHECK(filteredPeak(Biquad::Type::HighPass, 1000, 100) < 0.012f);
    CHECK(filteredPeak(Biquad::Type::HighPass, 100, 1000) == Approx(1.0).margin(0.02));
    CHECK(filteredPeak(Biquad::Type::LowPass, 1000, 10000) < 0.012f);
    CHECK(filteredPeak(Biquad::Type::LowPass, 10000, 1000) == Approx(1.0).margin(0.02));
}

TEST_CASE("pan law: constant power for mono, balance for stereo", "[mixd]") {
    auto c = panGains(0, false);
    CHECK(c.left == Approx(0.7071).margin(1e-3));
    CHECK(c.right == Approx(0.7071).margin(1e-3));
    auto l = panGains(-1, false);
    CHECK(l.left == Approx(1.0).margin(1e-5));
    CHECK(l.right == Approx(0.0).margin(1e-5));
    auto s = panGains(0, true);
    CHECK(s.left == 1.0f);
    CHECK(s.right == 1.0f);
    auto sr = panGains(0.5f, true);
    CHECK(sr.left == Approx(0.5));
    CHECK(sr.right == 1.0f);
    CHECK(dbToLinear(-144) == 0.0f);
    CHECK(dbToLinear(-6.0206) == Approx(0.5).margin(1e-4));
}

TEST_CASE("mixer: gain, pan, mute, routing through a track, sends and cue", "[mixd]") {
    Mixer m;
    MixPlan plan;
    plan.generation = 1;
    plan.inputs = {port("mic", 1), port("line", 2)};
    plan.buses = {outPort("spk"), outPort("hp")};

    SECTION("mono track at -6 dB, centred, to the master and the speakers") {
        auto t = track(1, 1, 0);
        t.gain = 0.5f;
        t.outputs = {{Destination::Kind::Master, 0}};
        plan.tracks = {t};
        plan.master.outputs = {{Destination::Kind::Bus, 0}};
        auto out = runMixer(m, plan, 0.8f);
        CHECK(out[0][0] == Approx(0.8 * 0.5 * 0.7071).margin(1e-3));
        CHECK(out[0][1] == Approx(0.8 * 0.5 * 0.7071).margin(1e-3));
        CHECK(out[1][0] == 0.0f);

        // mute fades to silence
        plan.generation = 2;
        plan.tracks[0].gain = 0;
        out = runMixer(m, plan, 0.8f);
        CHECK(std::fabs(out[0][0]) < 1e-5f);
    }

    SECTION("stereo track panned right, input from channel 2") {
        auto t = track(1, 2, 1);
        t.inputChannel = 1; // channel 2 of the line input; channel 3 does not exist and repeats channel 2
        t.pan = 1;
        t.outputs = {{Destination::Kind::Bus, 1}};
        plan.tracks = {t};
        auto out = runMixer(m, plan, 0.5f);
        CHECK(out[1][0] == Approx(0.0).margin(1e-4));
        CHECK(out[1][1] == Approx(0.5).margin(1e-3));
    }

    SECTION("track 2 feeds track 1, which goes to the master; sends and cue") {
        auto t2 = track(2, 2, 1);
        t2.outputs = {{Destination::Kind::Track, 1}};
        t2.sends = {{{Destination::Kind::Bus, 1}, 0.25f, false, 0}};
        t2.cue = true;
        auto t1 = track(1, 2, -1);
        t1.gain = 0.5f;
        t1.outputs = {{Destination::Kind::Master, 0}};
        plan.tracks = {t2, t1}; // sources first
        plan.master.outputs = {{Destination::Kind::Bus, 0}};
        plan.cueBus = 1;
        plan.cueGain = 0.5f;
        auto out = runMixer(m, plan, 0.4f);
        CHECK(out[0][0] == Approx(0.4 * 0.5).margin(1e-3));    // through track 1's fader
        CHECK(out[1][0] == Approx(0.4 * 0.25 + 0.4 * 0.5).margin(1e-3)); // post-fader send + cue (PFL)
    }

    SECTION("pre-fader send ignores the fader") {
        auto t = track(1, 2, 1);
        t.gain = 0;
        t.sends = {{{Destination::Kind::Bus, 1}, 1.0f, true, 0}};
        plan.tracks = {t};
        auto out = runMixer(m, plan, 0.3f);
        CHECK(out[1][0] == Approx(0.3).margin(1e-3));
    }
}

TEST_CASE("mixer writes the recorded strips to shared memory", "[mixd]") {
    std::vector<std::uint8_t> mem(shm::kLayout.totalBytes);
    shm::AudioRingWriter writer(mem.data(), 1);
    writer.configureSlot(0, 2, "master");
    writer.configureSlot(1, 1, "track/1");
    Mixer m;
    MixPlan plan;
    plan.generation = 1;
    plan.inputs = {port("mic", 1)};
    auto t = track(1, 1, 0);
    t.shmTap = Tap::Pre;
    t.gain = 0.5f;
    t.outputs = {{Destination::Kind::Master, 0}};
    plan.tracks = {t};
    plan.master.shmTap = Tap::Post;

    std::vector<float> in(256, 0.6f);
    const float* ptrs[1] = {in.data()};
    auto reader = shm::AudioRingReader::attach(mem.data(), mem.size(), nullptr);
    REQUIRE(reader);
    reader->seekToLatest();
    for (int b = 0; b < 100; ++b) {
        m.process(plan, 256, ptrs, &writer);
        writer.commit(256, b * 256);
    }
    std::vector<float> out(512);
    reader->seek(99 * 256);
    reader->read(1, out, 256, false);
    CHECK(out[0] == Approx(0.6));        // pre-fader, mono in the left half of the frame
    CHECK(out[1] == 0.0f);
    reader->read(0, out, 256, false);
    CHECK(out[0] == Approx(0.6 * 0.5 * 0.7071).margin(1e-3)); // master after the track fader and pan
}

TEST_CASE("plan builder: routing, solo, input state, shared memory, device demand", "[mixd]") {
    auto t = defaultTree(4);
    t.set("/track/1/input/kind", std::string("device"));
    t.set("/track/1/input/source", std::string("mic"));
    t.set("/track/2/input/kind", std::string("device"));
    t.set("/track/2/input/source", std::string("gone"));
    t.set("/track/3/input/kind", std::string("process"));
    t.set("/track/2/out/1/target", std::string("track/1"));
    t.set("/track/4/out/2/target", std::string("device/hp"));
    t.set("/master/out/1/target", std::string("device/spk"));
    t.set("/cue/target", std::string("device/hp"));
    t.set("/system/clock_device", std::string("usb"));
    t.set("/capture/audio/1/source", std::string("master"));
    t.set("/capture/audio/2/source", std::string("track/1"));
    t.set("/track/1/record/tap", std::string("pre"));
    t.set("/track/1/channels", std::int64_t{1});

    PortTable ports;
    ports.inputs["mic"] = port("mic", 2);
    ports.outputs["spk"] = outPort("spk");
    ports.outputs["hp"] = outPort("hp");

    auto r = buildPlan(t, ports, 7);
    const auto& p = *r.plan;
    CHECK(p.generation == 7);
    REQUIRE(p.tracks.size() == 4);
    // track 2 feeds track 1, so it is processed first
    auto pos = [&](int id) {
        for (std::size_t i = 0; i < p.tracks.size(); ++i)
            if (p.tracks[i].id == id) return static_cast<int>(i);
        return -1;
    };
    CHECK(pos(2) < pos(1));
    CHECK(r.inputState.at(1) == "ok");
    CHECK(r.inputState.at(2) == "disconnected");
    CHECK(r.inputState.at(3) == "disconnected");
    CHECK(r.inputState.at(4) == "none");
    CHECK(p.buses.size() == 2);
    CHECK(p.cueBus >= 0);
    CHECK(p.tracks[static_cast<std::size_t>(pos(4))].outputs.size() == 2); // master + hp
    CHECK(r.shmChannels[0] == 2);
    CHECK(r.shmChannels[1] == 1); // pre tap of a mono track
    CHECK(r.shmChannels[2] == 0);
    CHECK_FALSE(r.soloActive);

    auto d = deviceDemand(t);
    CHECK(d.inputs == std::vector<std::string>{"gone", "mic"});
    CHECK(d.outputs == std::vector<std::string>{"usb", "spk", "hp"});

    SECTION("solo keeps the soloed track, what feeds it and what it feeds") {
        t.set("/track/1/solo", true);
        auto s = buildPlan(t, ports, 8);
        CHECK(s.soloActive);
        for (const auto& tr : s.plan->tracks) {
            INFO("track " << tr.id);
            CHECK((tr.gain > 0) == (tr.id == 1 || tr.id == 2));
        }
    }
}

TEST_CASE("engine with fake devices: inputs reach the outputs, the clock follows the devices", "[mixd]") {
    FakeBackend backend;
    Engine engine(nullptr);
    DeviceManager devices(backend, engine, {});
    std::mutex mutex;
    DeviceManager::State last;
    devices.start([&](const DeviceManager::State& s) {
        std::lock_guard lock(mutex);
        last = s;
    });

    auto t = defaultTree(2);
    t.set("/track/1/input/kind", std::string("device"));
    t.set("/track/1/input/source", std::string("fake-line")); // 1 kHz, peak 0.25, -20 ppm
    t.set("/track/1/channels", std::int64_t{2});
    t.set("/master/out/1/target", std::string("device/fake-spk"));
    t.set("/master/out/2/target", std::string("device/fake-hp")); // +45 ppm, through a bridge
    std::uint64_t gen = 0;

    auto state = [&] {
        std::lock_guard lock(mutex);
        return last;
    };
    auto settle = [&](const std::string& clock) {
        for (int i = 0; i < 60; ++i) {
            devices.setDemand(deviceDemand(t));
            auto s = state();
            engine.setPlan(buildPlan(t, s.ports, ++gen).plan);
            engine.collectRetired();
            if (s.clockActive == clock && s.ports.inputs.size() == 1) return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    };

    REQUIRE(settle("device/fake-spk"));
    std::this_thread::sleep_for(1500ms); // bridges wait 20 packets before they play
    backend.takeOutputPeak("fake-spk");
    backend.takeOutputPeak("fake-hp");
    std::this_thread::sleep_for(500ms);
    CHECK(backend.takeOutputPeak("fake-spk") == Approx(0.25).margin(0.02));
    CHECK(backend.takeOutputPeak("fake-hp") == Approx(0.25).margin(0.02));

    // the clock device goes away: the headphones take over, the engine keeps running
    backend.setConnected("fake-spk", false);
    REQUIRE(settle("device/fake-hp"));
    std::this_thread::sleep_for(800ms);
    backend.takeOutputPeak("fake-hp");
    std::this_thread::sleep_for(300ms);
    CHECK(backend.takeOutputPeak("fake-hp") == Approx(0.25).margin(0.02));

    // nothing left: the internal timer keeps the engine going
    backend.setConnected("fake-hp", false);
    REQUIRE(settle("timer"));
    const auto before = engine.framesRendered();
    std::this_thread::sleep_for(300ms);
    CHECK(engine.framesRendered() - before > 9600);

    // back: the preferred device is the clock again
    backend.setConnected("fake-spk", true);
    backend.setConnected("fake-hp", true);
    REQUIRE(settle("device/fake-spk"));
    devices.stop();
}

TEST_CASE("exclusive mode: a device without a 48 kHz exclusive format runs shared", "[mixd]") {
    auto list = FakeBackend::standardDevices();
    for (auto& d : list)
        if (d.info.id == "fake-line") d.sharedOnly = true;
    FakeBackend backend(list);
    Engine engine(nullptr);
    DeviceManager devices(backend, engine, {});
    std::mutex mutex;
    DeviceManager::State last;
    devices.start([&](const DeviceManager::State& s) {
        std::lock_guard lock(mutex);
        last = s;
    });

    auto t = defaultTree(1);
    t.set("/system/audio_mode", std::string("exclusive"));
    t.set("/system/block_size", std::int64_t{128});
    t.set("/track/1/input/kind", std::string("device"));
    t.set("/track/1/input/source", std::string("fake-line"));
    t.set("/track/1/channels", std::int64_t{2});
    t.set("/master/out/1/target", std::string("device/fake-spk"));
    std::uint64_t gen = 0;
    DeviceManager::State s;
    for (int i = 0; i < 60; ++i) {
        devices.setDemand(deviceDemand(t));
        {
            std::lock_guard lock(mutex);
            s = last;
        }
        engine.setPlan(buildPlan(t, s.ports, ++gen).plan);
        engine.collectRetired();
        if (s.clockActive == "device/fake-spk" && s.ports.inputs.size() == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(s.ports.inputs.size() == 1);
    CHECK(s.errors.empty());
    REQUIRE(s.notes.size() == 1);
    CHECK(s.notes[0].find("Fake Line In") != std::string::npos);
    std::this_thread::sleep_for(1500ms);
    backend.takeOutputPeak("fake-spk");
    std::this_thread::sleep_for(500ms);
    CHECK(backend.takeOutputPeak("fake-spk") == Approx(0.25).margin(0.02));
    devices.stop();
}

TEST_CASE("exclusive mode: a device that ignores the period is reopened with its own", "[mixd]") {
    auto list = FakeBackend::standardDevices();
    for (auto& d : list)
        if (d.info.id == "fake-line") d.exclusivePeriod = 480;
    FakeBackend backend(list);
    Engine engine(nullptr);
    DeviceManager devices(backend, engine, {});
    std::mutex mutex;
    DeviceManager::State last;
    devices.start([&](const DeviceManager::State& s) {
        std::lock_guard lock(mutex);
        last = s;
    });

    auto t = defaultTree(1);
    t.set("/system/audio_mode", std::string("exclusive"));
    t.set("/system/block_size", std::int64_t{128});
    t.set("/track/1/input/kind", std::string("device"));
    t.set("/track/1/input/source", std::string("fake-line"));
    t.set("/track/1/channels", std::int64_t{2});
    t.set("/master/out/1/target", std::string("device/fake-spk"));
    std::uint64_t gen = 0;
    DeviceManager::State s;
    for (int i = 0; i < 80; ++i) {
        devices.setDemand(deviceDemand(t));
        {
            std::lock_guard lock(mutex);
            s = last;
        }
        engine.setPlan(buildPlan(t, s.ports, ++gen).plan);
        engine.collectRetired();
        if (s.ports.inputs.size() == 1 && s.notes.size() == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(s.notes.size() == 1);
    CHECK(s.notes[0].find("480") != std::string::npos);
    std::this_thread::sleep_for(1500ms);
    {
        std::lock_guard lock(mutex);
        s = last;
    }
    CHECK(s.notes.size() == 1); // stays open with 480: no further reopening
    backend.takeOutputPeak("fake-spk");
    std::this_thread::sleep_for(500ms);
    CHECK(backend.takeOutputPeak("fake-spk") == Approx(0.25).margin(0.02));
    devices.stop();
}

TEST_CASE("mixd behind the control server: settings, tracks, input state, shared memory, recording", "[mixd][integration]") {
    asio::io_context io;
    proto::ParamStore store;
    Controller* ctlPtr = nullptr;
    control::ControlServer::Options opt;
    opt.port = 0;
    opt.onDelta = [&](const proto::Delta& d) {
        if (ctlPtr) ctlPtr->onDelta(d);
    };
    opt.onClient = [&](const control::ClientInfo& c, bool connected) {
        if (ctlPtr) ctlPtr->onClient(c, connected);
    };
    control::ControlServer server(io, store, opt);
    FakeBackend backend;
    std::vector<std::uint8_t> mem(shm::kLayout.totalBytes);
    shm::AudioRingWriter writer(mem.data(), 1);
    Engine engine(&writer);
    DeviceManager devices(backend, engine, {});
    Controller::Options co;
    co.verbose = false;
    Controller ctl(io, server, devices, engine, &writer, co);
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
    REQUIRE(client.connect(3s));

    auto waitFor = [&](auto pred) {
        for (int i = 0; i < 100; ++i) {
            if (pred(client.tree())) return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    };

    // seeded: 4 tracks, master to the default output, which clocks the engine
    CHECK(client.tree().getAs<std::string>("/master/out/1/target") == "device/fake-spk");
    CHECK(client.tree().getAs<std::int64_t>("/mixer/track_count") == 4);
    CHECK(client.tree().getAs<std::string>("/devices/input/1/id") == "fake-mic");
    REQUIRE(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/system/clock_active") == "device/fake-spk"; }));

    REQUIRE(client.set({{"/track/1/input/kind", std::string("device")}, {"/track/1/input/source", std::string("fake-line")}})
                .get()
                .code == proto::ErrorCode::Ok);
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/track/1/input/state") == "ok"; }));

    // the master is recorded (slot 0): the line tone arrives there
    std::this_thread::sleep_for(1500ms);
    auto reader = shm::AudioRingReader::attach(mem.data(), mem.size(), nullptr);
    REQUIRE(reader);
    reader->seekToLatest(4800);
    std::vector<float> buf(4800 * 2);
    auto info = reader->read(0, buf, 4800);
    CHECK(info.status == shm::AudioRingReader::Status::Ok);
    float peak = 0;
    for (float v : buf) peak = std::max(peak, std::fabs(v));
    CHECK(peak == Approx(0.25).margin(0.03));
    CHECK(backend.takeOutputPeak("fake-spk") == Approx(0.25).margin(0.03));

    // a track from a missing device is "disconnected"; add and remove tracks
    REQUIRE(client.set({{"/track/2/input/kind", std::string("device")}, {"/track/2/input/source", std::string("nope")}})
                .get()
                .code == proto::ErrorCode::Ok);
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/track/2/input/state") == "disconnected"; }));
    auto added = client.call("/mixer/track/add", {{"channels", std::int64_t{1}}}).get();
    CHECK(std::get<std::int64_t>(added.value) == 5);
    CHECK(client.call("/mixer/track/remove", {{"track", std::int64_t{5}}}).get().code == proto::ErrorCode::Ok);

    // solo on track 3 silences the master (track 1 is not part of it)
    REQUIRE(client.set({{"/track/3/solo", true}}).get().code == proto::ErrorCode::Ok);
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<bool>("/mixer/solo_active") == true; }));
    std::this_thread::sleep_for(300ms);
    backend.takeOutputPeak("fake-spk");
    std::this_thread::sleep_for(200ms);
    CHECK(backend.takeOutputPeak("fake-spk") < 0.001f);

    // recording: the engine keeps the state, the capture process (capd) does the work
    CHECK(client.call("/capture/record/start").get().code == proto::ErrorCode::Conflict); // capd not running
    CHECK(client.call("/capture/report", {{"state", std::string("recording")}}).get().code ==
          proto::ErrorCode::Unauthorized);
    {
        control::ControlClient::Options co2;
        co2.port = server.port();
        co2.clientName = "capd";
        co2.role = proto::ClientRole::Service;
        control::ControlClient capd(co2);
        REQUIRE(capd.connect(3s));
        std::this_thread::sleep_for(100ms); // onClient runs after the snapshot was sent
        REQUIRE(client.call("/capture/record/start").get().code == proto::ErrorCode::Ok);
        CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/capture/state") == "starting"; }));
        CHECK(capd.call("/capture/report", {{"state", std::string("bogus")}}).get().code == proto::ErrorCode::BadRequest);
        REQUIRE(capd.call("/capture/report", {{"state", std::string("recording")}, {"file", std::string("C:/v/a.mkv")}})
                    .get()
                    .code == proto::ErrorCode::Ok);
        CHECK(waitFor([](const proto::ParamTree& t) {
            return t.getAs<std::string>("/capture/state") == "recording" && t.getAs<std::string>("/capture/file") == "C:/v/a.mkv" &&
                   t.getAs<std::int64_t>("/capture/elapsed_ms").value_or(0) > 0;
        }));
        // a new file segment (GPU reset): the file changes, the elapsed time goes on
        std::this_thread::sleep_for(300ms);
        REQUIRE(capd.call("/capture/report", {{"state", std::string("recording")},
                                              {"file", std::string("C:/v/a_2.mkv")},
                                              {"reason", std::string("gpu_reset")}})
                    .get()
                    .code == proto::ErrorCode::Ok);
        CHECK(waitFor([](const proto::ParamTree& t) {
            return t.getAs<std::string>("/capture/file") == "C:/v/a_2.mkv" && t.getAs<std::string>("/capture/state") == "recording";
        }));
        std::this_thread::sleep_for(300ms);
        CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::int64_t>("/capture/elapsed_ms").value_or(0) >= 500; }));
        // output states
        CHECK(client.call("/video/output/report", {{"output", std::string("vcam")}, {"state", std::string("active")}})
                  .get()
                  .code == proto::ErrorCode::Unauthorized);
        CHECK(capd.call("/video/output/report", {{"output", std::string("lan")}, {"state", std::string("active")}}).get().code ==
              proto::ErrorCode::BadRequest);
        CHECK(capd.call("/video/output/report", {{"output", std::string("vcam")}, {"state", std::string("bogus")}}).get().code ==
              proto::ErrorCode::BadRequest);
        REQUIRE(capd.call("/video/output/report", {{"output", std::string("vcam")}, {"state", std::string("waiting")}}).get().code ==
                proto::ErrorCode::Ok);
        REQUIRE(capd.call("/video/output/report", {{"output", std::string("monitor")},
                                                   {"state", std::string("no_display")},
                                                   {"detail", std::string("monitor not found: DISPLAY2")}})
                    .get()
                    .code == proto::ErrorCode::Ok);
        CHECK(waitFor([](const proto::ParamTree& t) {
            return t.getAs<std::string>("/video/vcam/state") == "waiting" &&
                   t.getAs<std::string>("/video/monitor/state") == "no_display" &&
                   t.getAs<std::string>("/video/monitor/detail") == "monitor not found: DISPLAY2";
        }));
        CHECK(client.call("/capture/record/start").get().code == proto::ErrorCode::Conflict);
        REQUIRE(client.call("/capture/record/stop").get().code == proto::ErrorCode::Ok);
        CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/capture/state") == "stopping"; }));
        REQUIRE(capd.call("/capture/report", {{"state", std::string("idle")}, {"duration_ms", std::int64_t{1234}}}).get().code ==
                proto::ErrorCode::Ok);
        CHECK(waitFor([](const proto::ParamTree& t) {
            return t.getAs<std::string>("/capture/state") == "idle" && t.getAs<std::int64_t>("/capture/elapsed_ms") == 1234;
        }));
        // capd goes away in the middle of a recording
        REQUIRE(client.call("/capture/record/start").get().code == proto::ErrorCode::Ok);
        REQUIRE(capd.call("/capture/report", {{"state", std::string("recording")}}).get().code == proto::ErrorCode::Ok);
        capd.disconnect();
    }
    CHECK(waitFor([](const proto::ParamTree& t) {
        return t.getAs<std::string>("/capture/state") == "error" &&
               t.getAs<std::string>("/capture/error") == "capture process disconnected" &&
               t.getAs<std::string>("/video/monitor/state") == "off" && t.getAs<std::string>("/video/monitor/detail") == "";
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
