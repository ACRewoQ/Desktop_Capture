// Lane A3: per-track recording (WAV / FLAC files, the take in the mixer) and the sampler (pad
// playback modes, file loading, the library folder), the save folders of recordings, plus both behind the
// control server with fake devices.

#include "audio_file.hpp"
#include "controller.hpp"
#include "device_manager.hpp"
#include "engine.hpp"
#include "fake_backend.hpp"
#include "mixer.hpp"
#include "plan_builder.hpp"
#include "recorder.hpp"
#include "sample_library.hpp"
#include "sampler.hpp"
#include "save_folder.hpp"

#include "dc/control/client.hpp"
#include "dc/protocol/defaults.hpp"
#include "dc/protocol/profile.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace dc;
using namespace dc::mixd;
using namespace std::chrono_literals;
using Catch::Approx;

namespace {

constexpr double kTwoPi = 6.283185307179586;

// A fresh, empty folder under the system temp directory.
std::filesystem::path tempFolder(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / ("dc_test_" + name);
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::vector<float> sine(double freq, double rate, std::size_t frames, int channels, float level) {
    std::vector<float> v(frames * static_cast<std::size_t>(channels));
    for (std::size_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            v[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] =
                level * static_cast<float>(std::sin(kTwoPi * freq * static_cast<double>(i) / rate));
    return v;
}

// Amplitude of freq in channel 0 of interleaved x (least squares over the middle part).
double amplitudeAt(const std::vector<float>& x, int channels, double freq, double rate) {
    const std::size_t frames = x.size() / static_cast<std::size_t>(channels);
    double s = 0, c = 0;
    std::size_t n = 0;
    for (std::size_t i = frames / 4; i < frames * 3 / 4; ++i, ++n) {
        const double ph = kTwoPi * freq * static_cast<double>(i) / rate;
        s += x[i * static_cast<std::size_t>(channels)] * std::sin(ph);
        c += x[i * static_cast<std::size_t>(channels)] * std::cos(ph);
    }
    return 2 * std::sqrt(s * s + c * c) / static_cast<double>(n);
}

std::shared_ptr<SampleData> rampSample(std::size_t frames) {
    auto d = std::make_shared<SampleData>();
    d->frames = frames;
    d->samples.resize(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        d->samples[2 * i] = static_cast<float>(i + 1) / 1000.0f;
        d->samples[2 * i + 1] = -static_cast<float>(i + 1) / 1000.0f;
    }
    return d;
}

std::vector<float> renderBus(Sampler& s, int bank, int frames) {
    s.render(frames);
    return std::vector<float>(s.bus(bank, 0), s.bus(bank, 0) + frames);
}

} // namespace

TEST_CASE("WAV writer: 24-bit PCM header, padding and round trip", "[mixd][recorder]") {
    const auto dir = tempFolder("wav");
    std::string err;
    {
        auto w = createAudioFile(dir / "mono.wav", AudioFileFormat::Wav, 1, &err);
        REQUIRE(w);
        const float x[3] = {0.5f, -0.5f, 1.5f}; // 1.5 clips
        REQUIRE(w->write(x, 3));
        REQUIRE(w->close());
    }
    const auto b = readBytes(dir / "mono.wav");
    REQUIRE(b.size() == 80 + 9 + 1); // header, 3 x 3 bytes, pad byte
    CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
    CHECK(le32(b, 4) == b.size() - 8);
    CHECK(std::memcmp(&b[12], "JUNK", 4) == 0);
    CHECK(std::memcmp(&b[48], "fmt ", 4) == 0);
    CHECK(le32(b, 60) == 48000);
    CHECK((b[70] | (b[71] << 8)) == 24);
    CHECK(std::memcmp(&b[72], "data", 4) == 0);
    CHECK(le32(b, 76) == 9);
    CHECK(b[80 + 6] == 0xFF); // +1.5 clipped to 0x7FFFFF
    CHECK(b[80 + 8] == 0x7F);

    {
        auto w = createAudioFile(dir / "stereo.wav", AudioFileFormat::Wav, 2, &err);
        REQUIRE(w);
        const auto x = sine(1000, 48000, 48000, 2, 0.5f);
        REQUIRE(w->write(x.data(), 48000));
        REQUIRE(w->writeSilence(100));
        CHECK(w->frames() == 48100);
        REQUIRE(w->close());
    }
    DecodedAudio back;
    REQUIRE(decodeAudioFile(dir / "stereo.wav", back, &err));
    REQUIRE(back.frames == 48100);
    const auto ref = sine(1000, 48000, 48000, 2, 0.5f);
    float maxErr = 0;
    for (std::size_t i = 0; i < ref.size(); ++i) maxErr = std::max(maxErr, std::fabs(ref[i] - back.samples[i]));
    CHECK(maxErr < 1e-6f); // 24 bit
    CHECK(back.samples[2 * 48050] == 0.0f);
}

TEST_CASE("FLAC writer round trip", "[mixd][recorder]") {
    const auto dir = tempFolder("flac");
    std::string err;
    const auto x = sine(440, 48000, 30000, 1, 0.8f);
    {
        auto w = createAudioFile(dir / "a.flac", AudioFileFormat::Flac, 1, &err);
        REQUIRE(w);
        REQUIRE(w->write(x.data(), 10000));
        REQUIRE(w->write(x.data() + 10000, 20000));
        REQUIRE(w->close());
    }
    DecodedAudio back;
    REQUIRE(decodeAudioFile(dir / "a.flac", back, &err));
    REQUIRE(back.frames == 30000);
    float maxErr = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        maxErr = std::max(maxErr, std::fabs(x[i] - back.samples[2 * i]));
        maxErr = std::max(maxErr, std::fabs(x[i] - back.samples[2 * i + 1])); // mono decodes to both sides
    }
    CHECK(maxErr < 1e-6f);
    CHECK(std::filesystem::file_size(dir / "a.flac") < 30000 * 3); // compressed
}

TEST_CASE("decoder refuses unknown and overlong files; resampler keeps the tone and removes aliases", "[mixd][sampler]") {
    const auto dir = tempFolder("decode");
    std::string err;
    {
        std::ofstream f(dir / "junk.bin", std::ios::binary);
        f << "this is not audio at all, just some text that is long enough";
    }
    DecodedAudio a;
    CHECK_FALSE(decodeAudioFile(dir / "junk.bin", a, &err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(decodeAudioFile(dir / "missing.wav", a, &err));

    {
        auto w = createAudioFile(dir / "long.wav", AudioFileFormat::Wav, 1, &err);
        REQUIRE(w->writeSilence(48000 * 3));
        REQUIRE(w->close());
    }
    CHECK_FALSE(decodeAudioFile(dir / "long.wav", a, &err, 2.0));
    CHECK(err.find("longer") != std::string::npos);

    // 44.1 kHz -> 48 kHz: a 1 kHz tone keeps its level and frequency
    const auto in441 = sine(1000, 44100, 44100, 1, 0.5f);
    const auto out = resampleInterleaved(in441, 1, 44100, 48000);
    CHECK(out.size() == Approx(48000).margin(2));
    CHECK(amplitudeAt(out, 1, 1000, 48000) == Approx(0.5).margin(0.005));
    // 96 kHz -> 48 kHz: 30 kHz cannot be represented and must not fold down to 18 kHz
    const auto in96 = sine(30000, 96000, 96000, 1, 0.5f);
    const auto down = resampleInterleaved(in96, 1, 96000, 48000);
    CHECK(amplitudeAt(down, 1, 18000, 48000) < 0.5 * 0.001); // < -60 dB
    const auto keep = resampleInterleaved(sine(5000, 96000, 96000, 1, 0.5f), 1, 96000, 48000);
    CHECK(amplitudeAt(keep, 1, 5000, 48000) == Approx(0.5).margin(0.005));
}

TEST_CASE("sampler: one-shot, toggle, hold, loop, stop, voice cut when the sample is replaced", "[mixd][sampler]") {
    Sampler s;
    auto table = std::make_shared<SamplerTable>();
    const auto ramp = rampSample(100);
    table->pads[0] = {ramp, PadMode::OneShot, 1.0f};              // bank 1 pad 1
    table->pads[1] = {ramp, PadMode::Toggle, 0.5f};               // bank 1 pad 2
    table->pads[2] = {ramp, PadMode::Hold, 1.0f};                 // bank 1 pad 3
    table->pads[padIndex(2, 1)] = {rampSample(10), PadMode::Loop, 1.0f}; // bank 2 pad 1
    s.adopt(table.get());

    // nothing playing: silence
    CHECK(renderBus(s, 0, 64)[10] == 0.0f);

    // one-shot plays the sample exactly once, into bank 1 and "all"
    REQUIRE(s.post({Sampler::Command::Type::Trigger, 0}));
    auto out = renderBus(s, 1, 64);
    CHECK(out[0] == Approx(0.001f));
    CHECK(out[63] == Approx(0.064f));
    CHECK(s.bus(0, 1)[63] == Approx(-0.064f));
    CHECK(s.bus(2, 0)[63] == 0.0f);
    CHECK((s.playing()[0] & 1u) == 1u);
    out = renderBus(s, 1, 64);
    CHECK(out[35] == Approx(0.1f));
    CHECK(out[36] == 0.0f); // ended after 100 frames
    CHECK((s.playing()[0] & 1u) == 0u);

    // an empty pad does nothing
    REQUIRE(s.post({Sampler::Command::Type::Trigger, 5}));
    CHECK(renderBus(s, 0, 64)[0] == 0.0f);

    // toggle: second press fades out within 5 ms; volume is applied
    REQUIRE(s.post({Sampler::Command::Type::Trigger, 1}));
    out = renderBus(s, 1, 10);
    CHECK(out[9] == Approx(0.005f));
    REQUIRE(s.post({Sampler::Command::Type::Trigger, 1}));
    out = renderBus(s, 1, 256);
    CHECK(out[100] == 0.0f);  // gone within 5 ms
    CHECK((s.playing()[0] & 2u) == 0u);
    REQUIRE(s.post({Sampler::Command::Type::Trigger, 1}));
    renderBus(s, 1, 10);
    CHECK((s.playing()[0] & 2u) != 0u);
    REQUIRE(s.post({Sampler::Command::Type::StopAll, 0}));
    renderBus(s, 1, 1024);
    CHECK(s.playing()[0] == 0u);

    // hold: release fades out
    REQUIRE(s.post({Sampler::Command::Type::Trigger, 2}));
    renderBus(s, 1, 10);
    REQUIRE(s.post({Sampler::Command::Type::Release, 2}));
    out = renderBus(s, 1, 10);
    CHECK(out[0] < 0.012f); // fading
    CHECK((s.playing()[0] & 4u) != 0u);

    // loop keeps going past the sample's end until pressed again; StopBank only stops its bank
    REQUIRE(s.post({Sampler::Command::Type::Trigger, static_cast<std::int16_t>(padIndex(2, 1))}));
    out = renderBus(s, 2, 25);
    CHECK(out[9] == Approx(0.010f));
    CHECK(out[10] == Approx(0.001f));
    CHECK(out[24] == Approx(0.005f));
    REQUIRE(s.post({Sampler::Command::Type::StopBank, 1}));
    renderBus(s, 2, 512);
    CHECK(((s.playing()[0] >> 16) & 1u) == 1u); // bank 2 pad 1 = index 16

    // a new table with a different sample for the looping pad cuts the voice at adoption
    auto table2 = std::make_shared<SamplerTable>(*table);
    table2->pads[padIndex(2, 1)].sample = rampSample(10);
    s.adopt(table2.get());
    table.reset(); // the old plan may be gone now
    renderBus(s, 0, 64);
    CHECK(((s.playing()[0] >> 16) & 1u) == 0u);
}

TEST_CASE("recorder take: every file starts and ends on the same block, removed strips record silence", "[mixd][recorder]") {
    const auto dir = tempFolder("take");
    std::string err;
    std::vector<RecordStreamSpec> specs = {{0, 2, Tap::Post, "00_Master.wav"},
                                           {1, 1, Tap::Pre, "01_Mono pre.wav"},
                                           {2, 2, Tap::Post, "02_Gone.wav"}};
    auto take = RecorderTake::create(dir, AudioFileFormat::Wav, specs, &err);
    REQUIRE(take);

    auto in = std::make_shared<InputPort>("in", 1, clocksync::ClockBridge::Config{});
    MixPlan plan;
    plan.generation = 1;
    plan.inputs.push_back(in);
    plan.take = take;
    StripPlan t1;
    t1.id = 1;
    t1.channels = 1;
    t1.input = 0;
    t1.gain = 0.5f; // pre tap ignores the fader
    t1.outputs.push_back({Destination::Kind::Master, 0});
    t1.recordStream = 1;
    t1.recordTap = Tap::Pre;
    StripPlan t2;
    t2.id = 2;
    t2.recordStream = 2;
    t2.recordTap = Tap::Post;
    plan.tracks = {t1, t2};
    plan.master.id = 0;
    plan.master.recordStream = 0;
    plan.master.recordTap = Tap::Post;

    Mixer m;
    std::vector<float> dc(256, 0.4f);
    const float* inputs[1] = {dc.data()};
    for (int b = 0; b < 100; ++b) m.process(plan, 256, inputs, nullptr);
    // track 2 removed mid-take
    MixPlan plan2 = plan;
    plan2.generation = 2;
    plan2.tracks = {t1};
    for (int b = 0; b < 100; ++b) m.process(plan2, 256, inputs, nullptr);
    CHECK(take->frames() == 200 * 256);
    REQUIRE(take->finish());
    CHECK(take->fileFrames() == 200 * 256);
    CHECK(take->dropped() == 0);

    // the strip files from the take
    DecodedAudio master, mono;
    REQUIRE(decodeAudioFile(dir / "00_Master.wav", master, &err));
    REQUIRE(decodeAudioFile(dir / pathFromUtf8("01_Mono pre.wav"), mono, &err));
    CHECK(master.frames == 200 * 256);
    CHECK(mono.frames == 200 * 256);
    CHECK(mono.samples[2 * 10000] == Approx(0.4f).margin(1e-5)); // pre fader
    // master: mono track at 0.5 with constant-power centre pan, settled
    CHECK(master.samples[2 * 40000] == Approx(0.4f * 0.5f * std::sqrt(0.5f)).margin(1e-3));
    // the file of the removed track kept going (silence) to the same length
    DecodedAudio gone;
    REQUIRE(decodeAudioFile(dir / "02_Gone.wav", gone, &err));
    CHECK(gone.frames == 200 * 256);
    CHECK(gone.samples[2 * 40000] == 0.0f);
}

TEST_CASE("recorder take: a full FIFO turns into counted silence, the length is kept", "[mixd][recorder]") {
    const auto dir = tempFolder("overflow");
    std::string err;
    auto take = RecorderTake::create(dir, AudioFileFormat::Wav, {{0, 2, Tap::Post, "m.wav"}}, &err);
    REQUIRE(take);
    // push far more than the FIFO holds in one go, faster than the disk thread can run
    std::vector<float> block(1024 * 2, 0.25f);
    const int blocks = static_cast<int>(RecorderTake::kFifoSeconds * 48000 / 1024) * 3;
    for (int i = 0; i < blocks; ++i) {
        take->push(0, block.data(), 1024);
        take->endBlock(1024);
    }
    REQUIRE(take->finish());
    CHECK(take->fileFrames() == static_cast<std::uint64_t>(blocks) * 1024);
    CHECK(take->dropped() % 1024 == 0); // whole blocks
}

TEST_CASE("plan builder: sampler input, recording streams follow the take", "[mixd][sampler][recorder]") {
    proto::ParamTree t;
    for (auto& c : proto::globalDefaults()) t.set(c.path, c.value);
    for (auto& c : proto::masterDefaults()) t.set(c.path, c.value);
    for (int n = 1; n <= 3; ++n) {
        for (auto& c : proto::trackDefaults(n)) t.set(c.path, c.value);
        t.set("/track/" + std::to_string(n) + "/position", std::int64_t{n});
    }
    t.set("/track/1/input/kind", std::string("sampler"));
    t.set("/track/2/input/kind", std::string("sampler"));
    t.set("/track/2/input/source", std::string("3"));
    t.set("/track/3/input/kind", std::string("sampler"));
    t.set("/track/3/input/source", std::string("9"));

    PlanResources res;
    res.sampler = std::make_shared<SamplerTable>();
    const auto dir = tempFolder("planbuild");
    std::string err;
    res.take = RecorderTake::create(dir, AudioFileFormat::Wav, {{2, 1, Tap::Pre, "a.wav"}, {0, 2, Tap::Post, "b.wav"}}, &err);
    REQUIRE(res.take);
    auto r = buildPlan(t, {}, 1, {}, res);
    auto byId = [&](int id) {
        for (auto& s : r.plan->tracks)
            if (s.id == id) return s;
        return StripPlan{};
    };
    CHECK(byId(1).samplerBank == 0);
    CHECK(byId(2).samplerBank == 3);
    CHECK(byId(3).samplerBank == -1);
    CHECK(r.inputState[1] == "ok");
    CHECK(r.inputState[3] == "disconnected");
    CHECK(byId(2).recordStream == 0);
    CHECK(byId(2).recordTap == Tap::Pre);
    CHECK(byId(1).recordStream == -1);
    CHECK(r.plan->master.recordStream == 1);
    CHECK(r.plan->take == res.take);
    res.take->finish();
}

TEST_CASE("mixd behind the control server: recorder and sampler", "[mixd][integration]") {
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
    REQUIRE(client.connect(3s));
    auto waitFor = [&](auto pred) {
        for (int i = 0; i < 100; ++i) {
            if (pred(client.tree())) return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    };
    auto ok = [](const proto::Result& r) { return r.code == proto::ErrorCode::Ok; };
    REQUIRE(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/system/clock_active") == "device/fake-spk"; }));

    const auto dir = tempFolder("integration");
    const auto dirText = pathToUtf8(dir);

    // --- recorder
    CHECK(client.call("/recorder/start").get().code == proto::ErrorCode::Conflict); // nothing armed
    CHECK(client.call("/recorder/stop").get().code == proto::ErrorCode::Conflict);
    REQUIRE(ok(client.set({{"/recorder/directory", dirText},
                           {"/recorder/folder_name", std::string("take")},
                           {"/track/1/input/kind", std::string("device")},
                           {"/track/1/input/source", std::string("fake-line")},
                           {"/track/1/name", std::string("Line: In")},
                           {"/track/1/record/enabled", true},
                           {"/track/1/record/tap", std::string("pre")},
                           {"/master/record/enabled", true}})
                   .get()));
    auto started = client.call("/recorder/start").get();
    REQUIRE(ok(started));
    const auto folder = std::get<std::string>(started.value);
    CHECK(folder == pathToUtf8(dir / "take"));
    CHECK(waitFor([](const proto::ParamTree& t) {
        return t.getAs<std::string>("/recorder/state") == "recording" && t.getAs<std::int64_t>("/recorder/elapsed_ms").value_or(0) > 300;
    }));
    CHECK(client.call("/recorder/start").get().code == proto::ErrorCode::Conflict);
    REQUIRE(ok(client.call("/recorder/stop").get()));
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/recorder/state") == "idle"; }));
    std::string err;
    DecodedAudio master, line;
    REQUIRE(decodeAudioFile(dir / "take" / "00_Master.wav", master, &err));
    REQUIRE(decodeAudioFile(dir / "take" / "01_Line_ In.wav", line, &err));
    CHECK(master.frames == line.frames);
    CHECK(master.frames > 48000 * 3 / 10);
    float peak = 0;
    for (std::size_t i = master.frames / 2 * 2; i < master.samples.size(); ++i) peak = std::max(peak, std::fabs(line.samples[i]));
    CHECK(peak > 0.05f); // the line tone was recorded
    CHECK(client.tree().getAs<std::int64_t>("/recorder/elapsed_ms").value_or(0) ==
          static_cast<std::int64_t>(master.frames * 1000 / 48000));

    // a second take gets its own folder
    REQUIRE(ok(client.call("/recorder/start").get()));
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/recorder/folder").value_or("").ends_with("take_2"); }));
    REQUIRE(ok(client.call("/recorder/stop").get()));

    // follows the video recording (with_capture, default on)
    {
        control::ControlClient::Options co2;
        co2.port = server.port();
        co2.clientName = "capd";
        co2.role = proto::ClientRole::Service;
        control::ControlClient capd(co2);
        REQUIRE(capd.connect(3s));
        std::this_thread::sleep_for(100ms);
        REQUIRE(ok(client.call("/capture/record/start").get()));
        CHECK(client.tree().getAs<std::string>("/recorder/state") != "recording"); // not before capd records
        REQUIRE(ok(capd.call("/capture/report", {{"state", std::string("recording")}}).get()));
        CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/recorder/state") == "recording"; }));
        CHECK(client.tree().getAs<std::string>("/recorder/folder").value_or("").ends_with("take_3"));
        REQUIRE(ok(client.call("/capture/record/stop").get()));
        CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/recorder/state") == "idle"; }));
        REQUIRE(ok(capd.call("/capture/report", {{"state", std::string("idle")}}).get()));
    }

    // --- sampler: a pad file, a sampler track to the speaker
    {
        auto w = createAudioFile(dir / "pad.wav", AudioFileFormat::Wav, 2, &err);
        const auto tone = sine(500, 48000, 48000 * 2, 2, 0.5f);
        REQUIRE(w->write(tone.data(), 48000 * 2));
        REQUIRE(w->close());
    }
    REQUIRE(ok(client.set({{"/track/1/input/kind", std::string("none")},
                           {"/track/2/input/kind", std::string("sampler")},
                           {"/track/2/input/source", std::string("2")},
                           {"/sampler/bank/2/pad/5/file", pathToUtf8(dir / "pad.wav")},
                           {"/sampler/bank/3/pad/1/file", pathToUtf8(dir / "nothing.wav")}})
                   .get()));
    CHECK(waitFor([](const proto::ParamTree& t) {
        return t.getAs<std::string>("/sampler/bank/2/pad/5/state") == "ready" &&
               t.getAs<std::int64_t>("/sampler/bank/2/pad/5/duration_ms") == 2000;
    }));
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/sampler/bank/3/pad/1/state") == "error"; }));
    CHECK(client.tree().getAs<std::string>("/track/2/input/state") == "ok");
    CHECK(client.tree().getAs<std::string>("/sampler/bank/1/name") == "Bank 1");

    std::this_thread::sleep_for(200ms);
    backend.takeOutputPeak("fake-spk");
    std::this_thread::sleep_for(200ms);
    CHECK(backend.takeOutputPeak("fake-spk") < 0.001f);
    CHECK(client.call("/sampler/trigger", {{"pad", std::int64_t{17}}}).get().code == proto::ErrorCode::OutOfRange);
    CHECK(client.call("/sampler/trigger", {{"pad", std::int64_t{5}}, {"bank", std::int64_t{9}}}).get().code ==
          proto::ErrorCode::OutOfRange);
    CHECK(client.call("/sampler/trigger").get().code == proto::ErrorCode::BadRequest);
    REQUIRE(ok(client.set({{"/sampler/active_bank", std::int64_t{2}}}).get()));
    REQUIRE(ok(client.call("/sampler/trigger", {{"pad", std::int64_t{5}}}).get())); // bank from active_bank
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<bool>("/sampler/bank/2/pad/5/playing") == true; }));
    std::this_thread::sleep_for(300ms);
    CHECK(backend.takeOutputPeak("fake-spk") == Approx(0.5).margin(0.05));
    REQUIRE(ok(client.call("/sampler/stop", {{"bank", std::int64_t{2}}}).get()));
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<bool>("/sampler/bank/2/pad/5/playing") == false; }));

    // replacing the file while it plays stops it; clearing it empties the pad
    REQUIRE(ok(client.call("/sampler/trigger", {{"pad", std::int64_t{5}}}).get()));
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<bool>("/sampler/bank/2/pad/5/playing") == true; }));
    REQUIRE(ok(client.set({{"/sampler/bank/2/pad/5/file", std::string()}}).get()));
    CHECK(waitFor([](const proto::ParamTree& t) {
        return t.getAs<std::string>("/sampler/bank/2/pad/5/state") == "empty" &&
               t.getAs<bool>("/sampler/bank/2/pad/5/playing") == false;
    }));

    client.disconnect();
    ctl.stop();
    server.stop();
    work.reset();
    net.join();
    devices.stop();
}

namespace {
void touch(const std::filesystem::path& p, std::size_t bytes = 10) {
    std::ofstream f(p, std::ios::binary);
    f << std::string(bytes, 'x');
}
} // namespace

TEST_CASE("sampler library: one folder level with folders first in natural order and audio files only", "[mixd][sampler]") {
    CHECK(isSampleFileName("a.wav"));
    CHECK(isSampleFileName("B.FLAC"));
    CHECK(isSampleFileName("c.Mp3"));
    CHECK(isSampleFileName("d.ogg"));
    CHECK_FALSE(isSampleFileName("e.txt"));
    CHECK_FALSE(isSampleFileName("wav"));
    CHECK_FALSE(isSampleFileName(".wav"));
    CHECK(naturalLess("Kick 9.wav", "kick 10.wav"));
    CHECK(naturalLess("a", "B"));
    CHECK_FALSE(naturalLess("kick 10.wav", "Kick 9.wav"));
    CHECK(naturalLess("x2", "x010"));

    const auto root = tempFolder("library");
    std::filesystem::create_directories(root / "Jingles" / "Old");
    std::filesystem::create_directories(root / "drums");
    std::filesystem::create_directories(root / ".hidden");
    touch(root / "Kick 10.wav", 1234);
    touch(root / "kick 9.flac");
    touch(root / "Applause.MP3");
    touch(root / "notes.txt");
    touch(root / "._Kick 10.wav");
    touch(root / "Jingles" / "Intro.ogg");
    touch(root / std::filesystem::path(u8"効果音.wav"));
    const auto rootText = pathToUtf8(root);

    auto l = listSampleLibrary(rootText, "", 0, 100);
    REQUIRE(l.code == proto::ErrorCode::Ok);
    REQUIRE(l.total == 6);
    std::vector<std::string> names;
    for (const auto& e : l.entries) names.push_back(e.name);
    CHECK(names == std::vector<std::string>{"drums", "Jingles", "Applause.MP3", "kick 9.flac", "Kick 10.wav", "効果音.wav"});
    CHECK(l.entries[0].folder);
    CHECK(l.entries[0].path.empty());
    CHECK_FALSE(l.entries[4].folder);
    CHECK(l.entries[4].size == 1234);
    CHECK(l.entries[4].path == pathToUtf8(root / "Kick 10.wav"));

    // paging
    auto page = listSampleLibrary(rootText, "", 4, 10);
    REQUIRE(page.entries.size() == 2);
    CHECK(page.offset == 4);
    CHECK(page.total == 6);
    CHECK(page.entries[0].name == "Kick 10.wav");
    CHECK(listSampleLibrary(rootText, "", 50, 10).entries.empty());

    // into a subfolder
    auto sub = listSampleLibrary(rootText, "Jingles", 0, 100);
    REQUIRE(sub.code == proto::ErrorCode::Ok);
    CHECK(sub.dir == "Jingles");
    REQUIRE(sub.entries.size() == 2);
    CHECK(sub.entries[0].name == "Old");
    CHECK(sub.entries[1].path == pathToUtf8(root / "Jingles" / "Intro.ogg"));
    CHECK(listSampleLibrary(rootText, "Jingles/Old", 0, 100).total == 0);

    const std::string json = libraryListingJson(sub);
    CHECK(json.rfind("{\"dir\":\"Jingles\",\"total\":2,\"offset\":0,\"truncated\":false,\"entries\":[{\"name\":\"Old\",\"folder\":true},", 0) == 0);

    // never outside the library, and clear errors
    CHECK(listSampleLibrary(rootText, "..", 0, 10).code == proto::ErrorCode::BadRequest);
    CHECK(listSampleLibrary(rootText, "Jingles/../..", 0, 10).code == proto::ErrorCode::BadRequest);
    CHECK(listSampleLibrary(rootText, "/etc", 0, 10).code == proto::ErrorCode::BadRequest);
    CHECK(listSampleLibrary(rootText, "Jingles/", 0, 10).code == proto::ErrorCode::BadRequest);
    CHECK(listSampleLibrary(rootText, "C:\\x", 0, 10).code == proto::ErrorCode::BadRequest);
    CHECK(listSampleLibrary(rootText, "nothing", 0, 10).code == proto::ErrorCode::NotFound);
    CHECK(listSampleLibrary(rootText, "notes.txt", 0, 10).code == proto::ErrorCode::NotFound);
    CHECK(listSampleLibrary("", "", 0, 10).code == proto::ErrorCode::Conflict);
    CHECK(listSampleLibrary("relative/dir", "", 0, 10).code == proto::ErrorCode::BadRequest);
    CHECK(listSampleLibrary(pathToUtf8(root / "missing"), "", 0, 10).code == proto::ErrorCode::NotFound);
}

TEST_CASE("mixd behind the control server: the sampler library is set only on the engine PC", "[mixd][integration]") {
    asio::io_context io;
    proto::ParamStore store;
    Controller* ctlPtr = nullptr;
    control::ControlServer::Options opt;
    opt.port = 0;
    opt.onDelta = [&](const proto::Delta& d) {
        if (ctlPtr) ctlPtr->onDelta(d);
    };
    control::ControlServer server(io, store, opt);
    FakeBackend backend;
    Engine engine(nullptr);
    DeviceManager devices(backend, engine, {});
    Controller::Options co;
    co.verbose = false;
    Controller ctl(io, server, devices, engine, nullptr, co);
    server.start();
    ctl.start(devices.enumerateNow());
    ctlPtr = &ctl;
    auto work = asio::make_work_guard(io);
    std::thread net([&] { io.run(); });

    control::ControlClient::Options o;
    o.port = server.port();
    o.clientName = "pc";
    control::ControlClient local(o);
    REQUIRE(local.connect(3s));
    o.clientName = "tablet";
    o.role = proto::ClientRole::Remote;
    control::ControlClient remote(o);
    REQUIRE(remote.connect(3s));
    auto waitFor = [&](control::ControlClient& c, auto pred) {
        for (int i = 0; i < 100; ++i) {
            if (pred(c.tree())) return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    };
    auto ok = [](const proto::Result& r) { return r.code == proto::ErrorCode::Ok; };

    const auto root = tempFolder("library_server");
    std::filesystem::create_directories(root / "Jingles");
    std::string err;
    {
        auto w = createAudioFile(root / "Jingles" / "hit.wav", AudioFileFormat::Wav, 2, &err);
        const auto tone = sine(500, 48000, 4800, 2, 0.5f);
        REQUIRE(w->write(tone.data(), 4800));
        REQUIRE(w->close());
    }
    const auto rootText = pathToUtf8(root);

    CHECK(local.call("/sampler/library/list").get().code == proto::ErrorCode::Conflict); // not set yet

    // the remote controller cannot change the folder (also not hidden among other changes)
    auto refused = remote.set({{"/sampler/library/folder", rootText}}).get();
    CHECK(refused.code == proto::ErrorCode::Unauthorized);
    CHECK(refused.path == "/sampler/library/folder");
    CHECK(remote.set({{"/sampler/bank/1/name", std::string("x")}, {"/sampler/library/folder", rootText}}).get().code ==
          proto::ErrorCode::Unauthorized);
    std::this_thread::sleep_for(100ms);
    CHECK(local.tree().getAs<std::string>("/sampler/bank/1/name") == "Bank 1");
    CHECK(local.tree().getAs<std::string>("/sampler/library/folder") == "");

    // the PC can; the remote sees it and lists the library
    REQUIRE(ok(local.set({{"/sampler/library/folder", rootText}}).get()));
    CHECK(waitFor(remote, [&](const proto::ParamTree& t) { return t.getAs<std::string>("/sampler/library/folder") == rootText; }));
    auto top = remote.call("/sampler/library/list").get();
    REQUIRE(ok(top));
    CHECK(std::get<std::string>(top.value) ==
          "{\"dir\":\"\",\"total\":1,\"offset\":0,\"truncated\":false,\"entries\":[{\"name\":\"Jingles\",\"folder\":true}]}");
    auto sub = remote.call("/sampler/library/list", {{"dir", std::string("Jingles")}}).get();
    REQUIRE(ok(sub));
    const auto hitPath = pathToUtf8(root / "Jingles" / "hit.wav");
    std::string hitJson; // as the JSON writes it (Windows backslashes are escaped)
    for (char ch : hitPath) hitJson += ch == '\\' ? std::string("\\\\") : std::string(1, ch);
    CHECK(std::get<std::string>(sub.value).find("\"path\":\"" + hitJson + "\"") != std::string::npos);
    CHECK(remote.call("/sampler/library/list", {{"dir", std::string("..")}}).get().code == proto::ErrorCode::BadRequest);
    CHECK(remote.call("/sampler/library/list", {{"count", std::int64_t{0}}}).get().code == proto::ErrorCode::OutOfRange);
    CHECK(remote.call("/sampler/library/list", {{"offset", std::int64_t{-1}}}).get().code == proto::ErrorCode::OutOfRange);

    // a path in the console's code page (CP932 "効果音" from a non-UTF-8 dc-cli) is refused, not loaded
    CHECK(local.set({{"/sampler/bank/1/pad/4/file", rootText + "/\x8c\xf8\x89\xca\x89\xb9.wav"}}).get().code ==
          proto::ErrorCode::TypeMismatch);

    // ...and assigns a file from it to a pad
    REQUIRE(ok(remote.set({{"/sampler/bank/1/pad/3/file", hitPath}}).get()));
    CHECK(waitFor(local, [](const proto::ParamTree& t) {
        return t.getAs<std::string>("/sampler/bank/1/pad/3/state") == "ready" &&
               t.getAs<std::int64_t>("/sampler/bank/1/pad/3/duration_ms") == 100;
    }));

    local.disconnect();
    remote.disconnect();
    ctl.stop();
    server.stop();
    work.reset();
    net.join();
}

TEST_CASE("save folder check: absolute paths on any drive, missing folders are made later, problems are named",
          "[mixd][recorder]") {
    const auto root = tempFolder("save_folder");
    const auto rootText = pathToUtf8(root);

    auto there = checkSaveFolder(rootText, "unused");
    CHECK(there.problem == "");
    CHECK(there.absolute);
    CHECK(there.exists);
    CHECK(there.writable);
    CHECK_FALSE(there.isDefault);
    REQUIRE(there.freeBytes);
    CHECK(*there.freeBytes > 0);
    // the write check leaves nothing behind
    CHECK(std::filesystem::is_empty(root));

    // not there yet: fine when the folder above can be written (the recording makes it)
    auto later = checkSaveFolder(pathToUtf8(root / "new" / "deeper"), "unused");
    CHECK(later.problem == "");
    CHECK_FALSE(later.exists);
    CHECK(later.writable);
    CHECK(later.freeBytes);
    CHECK_FALSE(std::filesystem::exists(root / "new"));

    // "" is the default folder
    auto byDefault = checkSaveFolder("", rootText);
    CHECK(byDefault.isDefault);
    CHECK(byDefault.path == rootText);
    CHECK(byDefault.exists);

    CHECK(checkSaveFolder("Videos", rootText).problem == "not_absolute");
    CHECK(checkSaveFolder("Videos/rec", rootText).problem == "not_absolute");
#ifdef _WIN32
    CHECK(checkSaveFolder("D:Videos", rootText).problem == "not_absolute"); // drive-relative
    CHECK(checkSaveFolder("\\Videos", rootText).problem == "not_absolute"); // no drive
    CHECK(checkSaveFolder("D:/Videos", rootText).absolute);                 // either separator
#endif
    {
        std::ofstream f(root / "taken");
        f << "x";
    }
    CHECK(checkSaveFolder(pathToUtf8(root / "taken"), "").problem == "not_folder");
    CHECK(checkSaveFolder(pathToUtf8(root / "taken" / "below"), "").problem == "not_folder");

    const std::string json = folderCheckJson(later);
    CHECK(json.find("\"exists\":false") != std::string::npos);
    CHECK(json.find("\"writable\":true") != std::string::npos);
    CHECK(json.find("\"default\":false") != std::string::npos);
    CHECK(json.find("\"free_bytes\":") != std::string::npos);
    CHECK(json.find("\"problem\":\"\"") != std::string::npos);
    std::filesystem::remove_all(root);
}

TEST_CASE("mixd behind the control server: save folders are set on the engine PC and checked", "[mixd][integration]") {
    for (const char* path : {"/capture/directory", "/recorder/directory"}) {
        const auto* spec = proto::schemaV1().find(path);
        REQUIRE(spec);
        CHECK(spec->access == proto::Access::LocalOnly);
        CHECK(proto::inProfile(*spec)); // kept in profiles like before
    }

    asio::io_context io;
    proto::ParamStore store;
    Controller* ctlPtr = nullptr;
    control::ControlServer::Options opt;
    opt.port = 0;
    opt.onDelta = [&](const proto::Delta& d) {
        if (ctlPtr) ctlPtr->onDelta(d);
    };
    control::ControlServer server(io, store, opt);
    FakeBackend backend;
    Engine engine(nullptr);
    DeviceManager devices(backend, engine, {});
    Controller::Options co;
    co.verbose = false;
    Controller ctl(io, server, devices, engine, nullptr, co);
    server.start();
    ctl.start(devices.enumerateNow());
    ctlPtr = &ctl;
    auto work = asio::make_work_guard(io);
    std::thread net([&] { io.run(); });

    control::ControlClient::Options o;
    o.port = server.port();
    o.clientName = "pc";
    control::ControlClient local(o);
    REQUIRE(local.connect(3s));
    o.clientName = "tablet";
    o.role = proto::ClientRole::Remote;
    control::ControlClient remote(o);
    REQUIRE(remote.connect(3s));
    auto waitFor = [&](control::ControlClient& c, auto pred) {
        for (int i = 0; i < 100; ++i) {
            if (pred(c.tree())) return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    };
    auto ok = [](const proto::Result& r) { return r.code == proto::ErrorCode::Ok; };

    const auto root = tempFolder("save_folder_server");
    const auto videos = pathToUtf8(root / "Videos");
    const auto takes = pathToUtf8(root / "Takes");

    // a remote controller cannot move the files on the engine's PC
    for (const char* path : {"/capture/directory", "/recorder/directory"}) {
        auto refused = remote.set({{path, videos}}).get();
        CHECK(refused.code == proto::ErrorCode::Unauthorized);
        CHECK(refused.path == path);
    }
    REQUIRE(ok(local.set({{"/capture/directory", videos}, {"/recorder/directory", takes}}).get()));
    CHECK(waitFor(remote, [&](const proto::ParamTree& t) { return t.getAs<std::string>("/recorder/directory") == takes; }));

    // ...but it can see what the folder would do
    auto check = remote.call("/system/folder/check", {{"for", std::string("capture")}}).get();
    REQUIRE(ok(check));
    const auto json = std::get<std::string>(check.value);
    std::string videosJson; // as the JSON writes it (Windows backslashes are escaped)
    for (char ch : videos) videosJson += ch == '\\' ? std::string("\\\\") : std::string(1, ch);
    CHECK(json.find("\"path\":\"" + videosJson + "\"") != std::string::npos);
    CHECK(json.find("\"exists\":false") != std::string::npos); // made when the recording starts
    CHECK(json.find("\"problem\":\"\"") != std::string::npos);
    auto other = local.call("/system/folder/check", {{"for", std::string("recorder")}, {"path", std::string("rel")}}).get();
    REQUIRE(ok(other));
    CHECK(std::get<std::string>(other.value).find("\"problem\":\"not_absolute\"") != std::string::npos);
    auto byDefault = local.call("/system/folder/check", {{"for", std::string("recorder")}, {"path", std::string()}}).get();
    REQUIRE(ok(byDefault));
    CHECK(std::get<std::string>(byDefault.value).find("\"default\":true") != std::string::npos);
    CHECK(local.call("/system/folder/check", {{"for", std::string("video")}}).get().code == proto::ErrorCode::BadRequest);
    CHECK(local.call("/system/folder/check").get().code == proto::ErrorCode::BadRequest);

    // a take goes into the folder (made on the way), a relative folder is refused before anything is written
    REQUIRE(ok(local.set({{"/master/record/enabled", true}, {"/recorder/folder_name", std::string("take")}}).get()));
    auto started = local.call("/recorder/start").get();
    REQUIRE(ok(started));
    CHECK(std::get<std::string>(started.value) == pathToUtf8(root / "Takes" / "take"));
    REQUIRE(ok(local.call("/recorder/stop").get()));
    CHECK(waitFor(local, [](const proto::ParamTree& t) { return t.getAs<std::string>("/recorder/state") == "idle"; }));
    CHECK(std::filesystem::exists(root / "Takes" / "take" / "00_Master.wav"));

    REQUIRE(ok(local.set({{"/recorder/directory", std::string("Takes")}}).get()));
    auto refused = local.call("/recorder/start").get();
    CHECK(refused.code == proto::ErrorCode::BadRequest);
    CHECK(refused.message.find("absolute") != std::string::npos);
    CHECK(waitFor(local, [](const proto::ParamTree& t) {
        return t.getAs<std::string>("/recorder/state") == "error" &&
               t.getAs<std::string>("/recorder/error").value_or("").find("absolute") != std::string::npos;
    }));

    local.disconnect();
    remote.disconnect();
    ctl.stop();
    server.stop();
    work.reset();
    net.join();
    std::filesystem::remove_all(root);
}
