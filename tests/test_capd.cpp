// capd's portable parts: settings from the parameter tree, and the audio time line (shared memory -> recording).

#include "audio_capture.hpp"
#include "audio_encoder.hpp"
#include "avsync.hpp"
#include "muxer.hpp"
#include "engine_follower.hpp"
#include "mock_engine.hpp"
#include "recorder.hpp"
#include "settings.hpp"

#include "dc/control/client.hpp"

#include "dc/protocol/defaults.hpp"
#include "dc/shmaudio/clock.hpp"
#include "dc/shmaudio/ring.hpp"
#include "dc/shmaudio/shared_memory.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
static unsigned long testPid() { return GetCurrentProcessId(); }
#else
#include <unistd.h>
static unsigned long testPid() { return static_cast<unsigned long>(getpid()); }
#endif

using namespace dc;
using namespace dc::capd;

TEST_CASE("audio sources map to shared-memory slots", "[capd]") {
    CHECK(slotForSource("master") == 0u);
    CHECK(slotForSource("track/1") == 1u);
    CHECK(slotForSource("track/64") == 64u);
    CHECK_FALSE(slotForSource(""));
    CHECK_FALSE(slotForSource("track/0"));
    CHECK_FALSE(slotForSource("track/65"));
    CHECK_FALSE(slotForSource("track/01"));
    CHECK_FALSE(slotForSource("track/"));
    CHECK_FALSE(slotForSource("track/2x"));
}

TEST_CASE("recording settings come from the engine's tree", "[capd]") {
    proto::ParamTree tree;
    tree.set("/capture/mix", std::int64_t{2});
    tree.set("/capture/container", std::string("mp4"));
    tree.set("/capture/video/codec", std::string("hevc"));
    tree.set("/capture/video/width", std::int64_t{1281});  // odd sizes are made even
    tree.set("/capture/video/height", std::int64_t{720});
    tree.set("/capture/video/fps", std::int64_t{30});
    tree.set("/capture/audio/1/source", std::string("master"));
    tree.set("/capture/audio/2/source", std::string(""));
    tree.set("/capture/audio/3/source", std::string("track/5"));
    tree.set("/capture/audio/3/codec", std::string("pcm"));
    tree.set("/video/mix/2/width", std::int64_t{2560});
    tree.set("/video/mix/2/height", std::int64_t{1440});
    tree.set("/track/5/name", std::string("Game"));

    std::string err;
    auto s = settingsFromTree(tree, &err);
    REQUIRE(s);
    CHECK(s->container == "mp4");
    CHECK(s->video.codec == "hevc");
    CHECK(s->video.width == 1280);
    CHECK(s->video.fps == 30);
    CHECK(s->mix.width == 2560);
    CHECK(s->mix.height == 1440);
    REQUIRE(s->audio.size() == 2);
    CHECK(s->audio[0].slot == 0);
    CHECK(s->audio[0].codec == "aac");
    CHECK(s->audio[1].slot == 5);
    CHECK(s->audio[1].label == "track/5");
    CHECK(s->audio[1].codec == "pcm");
    CHECK(s->audio[0].title.empty());  // no /master/name in this tree
    CHECK(s->audio[1].title == "Game");

    tree.set("/capture/mix", std::int64_t{0});
    CHECK_FALSE(settingsFromTree(tree, &err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("defaults of the schema give a 1080p60 H.264 recording ", "[capd]") {
    proto::ParamTree tree;
    for (const auto& c : proto::globalDefaults()) tree.set(c.path, c.value);
    std::string err;
    auto s = settingsFromTree(tree, &err);
    REQUIRE(s);
    CHECK(s->video.width == 1920);
    CHECK(s->video.height == 1080);
    CHECK(s->video.fps == 60);
    CHECK(s->video.codec == "h264");
}

TEST_CASE("4K H.264 is recorded as HEVC", "[capd]") {
    VideoSettings v;
    bool changed = true;
    CHECK(effectiveCodec(v, &changed) == "h264");
    CHECK_FALSE(changed);
    v.width = 3840;
    v.height = 2160;
    CHECK(effectiveCodec(v, &changed) == "hevc");
    CHECK(changed);
    v.codec = "av1";
    CHECK(effectiveCodec(v, &changed) == "av1");
}

TEST_CASE("recording path uses the folder, the pattern and the container", "[capd]") {
    RecordSettings s;
    s.directory = "/rec";
    s.filenamePattern = "cap_%Y:%m";  // ':' is not allowed in Windows file names
    std::tm tm{};
    tm.tm_year = 126;
    tm.tm_mon = 9;
    tm.tm_mday = 23;
    tm.tm_hour = 12;
    const std::time_t t = std::mktime(&tm);
    CHECK(recordingPath(s, t) == "/rec/cap_2026_10.mkv");
}

namespace {

// Engine stand-in: writes a ramp (sample value = engine frame number) and stamps each block with a synthetic
// QPC time, so the test controls the clock completely.
struct FakeEngine {
    explicit FakeEngine(void* mem) : writer(mem, 1) {
        writer.configureSlot(0, 2, "master");
        writer.configureSlot(1, 1, "track/1");
    }
    // Commits frames up to (but not beyond) the time `qpc`, starting from `baseQpc` for engine frame 0.
    void runUntil(std::int64_t qpc) {
        const std::int64_t f = shm::qpcFrequency();
        for (;;) {
            const std::uint64_t frame = writer.framesWritten();
            const std::int64_t blockQpc =
                baseQpc + static_cast<std::int64_t>(static_cast<double>(frame) * double(f) / rate);
            if (blockQpc + static_cast<std::int64_t>(double(f) * kBlock / rate) > qpc) return;
            std::vector<float> st(kBlock * 2), mono(kBlock);
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                st[i * 2] = st[i * 2 + 1] = static_cast<float>(frame + i);
                mono[i] = -static_cast<float>(frame + i);
            }
            writer.write(0, st.data(), kBlock, 2);
            writer.write(1, mono.data(), kBlock, 1);
            // Timestamp noise as a real engine's callback times have it (uniform +-jitterMs).
            const auto noise = static_cast<std::int64_t>(jitterMs * 1e-3 * double(f) *
                                                         (double(std::rand()) / RAND_MAX * 2 - 1));
            writer.commit(kBlock, blockQpc + noise);
        }
    }
    static constexpr std::uint32_t kBlock = 480;
    shm::AudioRingWriter writer;
    std::int64_t baseQpc = 0;
    double rate = shm::kSampleRate;  // the engine device's real sample rate measured in QPC time
    double jitterMs = 0;
};

// A skip or repeat changes the step by a whole frame; the float sums of the 64-tap interpolator over a ramp that
// large wobble by a few millionths of its value.
float rampTolerance(float value) { return 0.05f + 1e-5f * std::fabs(value); }

struct Collected {
    std::vector<float> master, track;  // left channel per frame
    std::vector<float> trackRight;
};

}  // namespace

TEST_CASE("audio follows the recording's time line across engine stops and restarts", "[capd]") {
    const std::string suffix = ".capd-test." + std::to_string(testPid());
    std::string err;
    auto mem = shm::SharedMemory::create(shm::kLayout.totalBytes, suffix, &err);
    REQUIRE(mem.valid());

    const std::int64_t f = shm::qpcFrequency();
    const std::int64_t base = shm::nowQpc();
    auto engine = std::make_unique<FakeEngine>(mem.data());
    engine->baseQpc = base;
    engine->runUntil(base + f);  // one second of audio before the recording starts

    Collected out;
    AudioCapture cap({0, 1}, suffix);
    const std::int64_t start = base + f / 2;  // recording time zero = engine frame 24000
    cap.begin(start, [&](const std::vector<std::span<const float>>& s, std::uint32_t frames) {
        for (std::uint32_t i = 0; i < frames; ++i) {
            out.master.push_back(s[0][i * 2]);
            out.track.push_back(s[1][i * 2]);
            out.trackRight.push_back(s[1][i * 2 + 1]);
        }
    });

    // Run 2 s of engine time in 10 ms steps.
    std::int64_t now = base + f;
    for (int step = 0; step < 100; ++step) {
        now += f / 100;
        engine->runUntil(now);
        cap.pump(now, AudioCapture::kLagFrames);
    }
    REQUIRE(out.master.size() > 48000);
    // The clock correction resamples (ratio within a few ppm of 1 here), so the ramp comes back within a small
    // fraction of a step (rampTolerance).
    CHECK(std::fabs(out.master[0] - 24000.0f) <= 1.0f);
    for (std::size_t i = 1; i < out.master.size(); ++i) REQUIRE(std::fabs(out.master[i] - out.master[i - 1] - 1.0f) < rampTolerance(out.master[i]));
    CHECK(std::fabs(out.track[100] + out.master[100]) < 0.01f);
    CHECK(out.trackRight[100] == out.track[100]);  // mono track copied to both channels
    CHECK(cap.stats().silenceFrames == 0);
    CHECK(cap.stats().engineConnected);

    // Engine stops for 1.5 s: silence keeps the time line going.
    const std::size_t beforeStop = out.master.size();
    for (int step = 0; step < 150; ++step) {
        now += f / 100;
        cap.pump(now, AudioCapture::kLagFrames);
    }
    CHECK_FALSE(cap.stats().engineConnected);
    CHECK(cap.stats().silenceFrames > 48000);

    // Engine restarts: frame numbers start from 0 again, at a new QPC base.
    engine.reset();
    engine = std::make_unique<FakeEngine>(mem.data());
    engine->baseQpc = now;
    for (int step = 0; step < 100; ++step) {
        now += f / 100;
        engine->runUntil(now);
        cap.pump(now, AudioCapture::kLagFrames);
    }
    CHECK(cap.stats().engineConnected);

    // Every output frame is either silence or the engine frame recorded at that output frame's time.
    const auto expectedTotal = static_cast<double>(now - start) / double(f) * 48000.0 - AudioCapture::kLagFrames;
    CHECK(std::fabs(double(out.master.size()) - expectedTotal) <= 2.0);
    std::size_t aligned = 0;
    for (std::size_t p = beforeStop; p < out.master.size(); ++p) {
        if (out.master[p] == 0.0f) continue;
        // Frame v was stamped at base + v / 48 kHz (the first engine's tail is read just after the stop began).
        const double v = double(out.master[p]) / 48000.0;
        const double tOut = double(p) / 48000.0;
        const double tFirst = double(base - start) / double(f) + v;
        const double tRestarted = double(engine->baseQpc - start) / double(f) + v;
        REQUIRE(std::min(std::fabs(tFirst - tOut), std::fabs(tRestarted - tOut)) < 0.002);
        ++aligned;
    }
    CHECK(aligned > 30000);

    mem.reset();
}

TEST_CASE("audio follows an engine clock that drifts from QPC without jumps", "[capd]") {
    const std::string suffix = ".capd-drift." + std::to_string(testPid());
    std::string err;
    auto mem = shm::SharedMemory::create(shm::kLayout.totalBytes, suffix, &err);
    REQUIRE(mem.valid());

    // The engine's device runs 600 ppm fast (and its timestamps jitter by 1 ms): in 3 minutes it gets 108 ms ahead of QPC. Re-aligning at 40 ms (P2)
    // would skip audio twice; the resampler must follow instead.
    const double ppm = 600;
    const std::int64_t f = shm::qpcFrequency();
    const std::int64_t base = shm::nowQpc();
    FakeEngine engine(mem.data());
    engine.baseQpc = base;
    engine.rate = shm::kSampleRate * (1 + ppm * 1e-6);
    engine.jitterMs = 1.0;
    engine.runUntil(base + f);

    std::vector<float> master;
    AudioCapture cap({0}, suffix);
    const std::int64_t start = base + f / 2;
    cap.begin(start, [&](const std::vector<std::span<const float>>& s, std::uint32_t frames) {
        for (std::uint32_t i = 0; i < frames; ++i) master.push_back(s[0][i * 2]);
    });
    std::int64_t now = base + f;
    double worstMs = 0;
    for (int step = 0; step < 18000; ++step) {  // 180 s in 10 ms steps
        now += f / 100;
        engine.runUntil(now);
        cap.pump(now, AudioCapture::kLagFrames);
        if (step > 3000) worstMs = std::max(worstMs, std::fabs(cap.stats().driftMs));
    }
    const auto st = cap.stats();
    CHECK(st.resyncs == 1);  // the first alignment only
    CHECK(st.silenceFrames == 0);
    CHECK(std::fabs(st.ratio - (1 + ppm * 1e-6)) < 50e-6);
    CHECK(worstMs < 2.0);
    // Continuous: each output frame advances the ramp by the ratio (no skip, no repeat).
    REQUIRE(master.size() > 48000 * 170);
    for (std::size_t i = 1; i < master.size(); ++i) REQUIRE(std::fabs(master[i] - master[i - 1] - 1.0f) < rampTolerance(master[i]));
    // Aligned: the engine frame heard at output frame p was recorded at p's time.
    const std::size_t p = master.size() - 1;
    const double recordedAt = double(base - start) / double(f) + double(master[p]) / engine.rate;
    CHECK(std::fabs(recordedAt - double(p) / 48000.0) < 0.002);
    mem.reset();
}

// --- capd following the engine (mock engine in capd-delegating mode, protocol.md 5.4.1) ---


namespace {

std::string waitForState(control::ControlClient& c, const std::string& want, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    std::string state;
    while (std::chrono::steady_clock::now() < end) {
        state = c.tree().getAs<std::string>("/capture/state").value_or("");
        if (state == want) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return state;
}

}  // namespace

TEST_CASE("capd records when the engine says so and reports back", "[capd]") {
    asio::io_context io;
    proto::ParamStore store;
    control::ControlServer::Options so;
    so.port = 0;
    control::ControlServer server(io, store, so);
    mock::MockEngine::Options mo;
    mo.meters = false;
    mo.delegateCapture = true;
    mock::MockEngine engine(io, server, mo);
    server.start();
    engine.start();
    auto work = asio::make_work_guard(io);
    std::thread ioThread([&] { io.run(); });

    const auto dir = std::filesystem::temp_directory_path() / ("capd-follow-" + std::to_string(testPid()));
    std::filesystem::remove_all(dir);

    std::atomic<bool> stop{false};
    EngineFollower::Options fo;
    fo.port = server.port();
    fo.withVideo = false;
    fo.shmSuffix = ".capd-follow." + std::to_string(testPid());  // no engine audio: silence
    EngineFollower follower(fo);
    std::thread followerThread([&] { follower.run(stop); });

    control::ControlClient::Options co;
    co.port = server.port();
    co.clientName = "ui";
    control::ControlClient ui(co);
    REQUIRE(ui.connect(std::chrono::seconds(2)));
    // Wait until capd is connected: before that, "starting" would only be picked up late.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    REQUIRE(ui.set({{"/capture/directory", dir.string()}}).get().code == proto::ErrorCode::Ok);
    REQUIRE(ui.call("/capture/record/start").get().code == proto::ErrorCode::Ok);
    REQUIRE(waitForState(ui, "recording", std::chrono::seconds(3)) == "recording");
    const auto file = ui.tree().getAs<std::string>("/capture/file").value_or("");
    CHECK(file.find(dir.string()) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    REQUIRE(ui.call("/capture/record/stop").get().code == proto::ErrorCode::Ok);
    REQUIRE(waitForState(ui, "idle", std::chrono::seconds(3)) == "idle");
    CHECK(ui.tree().getAs<std::int64_t>("/capture/elapsed_ms").value_or(0) >= 400);
    CHECK(std::filesystem::file_size(std::filesystem::path(file)) > 1000);

    // A recording capd cannot start (the folder is a file) ends in the error state with the reason.
    const auto blocker = dir / "not-a-folder";
    { std::ofstream(blocker) << "x"; }
    REQUIRE(ui.set({{"/capture/directory", (blocker / "sub").string()}}).get().code == proto::ErrorCode::Ok);
    REQUIRE(ui.call("/capture/record/start").get().code == proto::ErrorCode::Ok);
    REQUIRE(waitForState(ui, "error", std::chrono::seconds(3)) == "error");
    CHECK_FALSE(ui.tree().getAs<std::string>("/capture/error").value_or("").empty());

    // Only a service client may report.
    CHECK(ui.call("/capture/report", {{"state", std::string("idle")}}).get().code ==
          proto::ErrorCode::Unauthorized);

    ui.disconnect();
    stop = true;
    followerThread.join();
    engine.stop();
    server.stop();
    work.reset();
    asio::post(io, [&] { io.stop(); });
    ioThread.join();
    std::filesystem::remove_all(dir);
}

// --- disk space (design doc "保存先のディスク": stop safely while the file can still be finalized) ---

TEST_CASE("the disk reserve covers 30 seconds of the recording, at least 256 MB", "[capd]") {
    RecordSettings s;
    s.audio.push_back({0, "master", "", "aac", 192});
    s.video.bitrateKbps = 100000;  // VBR peaks at 150 Mbit/s: 30 s = 563 MB plus the audio
    CHECK(diskReserveBytes(s) == (150000ull + 192) * 1000 / 8 * 30);
    s.video.bitrateKbps = 8000;
    CHECK(diskReserveBytes(s) == 256ull << 20);
    s.withVideo = false;
    CHECK(diskReserveBytes(s) == 256ull << 20);
}

TEST_CASE("a recording refuses to start on a nearly full disk and stops before it fills up", "[capd]") {
    const auto dir = std::filesystem::temp_directory_path() / ("capd-disk-" + std::to_string(testPid()));
    std::filesystem::create_directories(dir);
    RecordSettings s;
    s.withVideo = false;
    s.audio.push_back({0, "master", "", "aac", 192});
    const std::string path = (dir / "rec.mp4").string();
    s.container = "mp4";
    const std::uint64_t reserve = diskReserveBytes(s);

    {
        Recorder rec(s, path, ".capd-disk-none");
        rec.setFreeSpaceProbe([&](const std::string&) { return std::optional<std::uint64_t>(reserve); });
        CHECK_THROWS_WITH(rec.start(), Catch::Matchers::ContainsSubstring("not enough free disk space"));
    }

    std::atomic<std::uint64_t> free{reserve * 4};
    Recorder rec(s, path, ".capd-disk-none");
    rec.setFreeSpaceProbe([&](const std::string&) { return std::optional<std::uint64_t>(free.load()); });
    rec.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(rec.status().error.empty());
    CHECK(rec.status().diskFreeBytes == reserve * 4);
    free = reserve - 1;
    std::string error;
    for (int i = 0; i < 40 && error.empty(); ++i) {  // checked once a second
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        error = rec.status().error;
    }
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("disk almost full"));
    const auto fin = rec.stop();
    CHECK_THAT(fin.error, Catch::Matchers::ContainsSubstring("disk almost full"));

    // The MP4 was finalized (its index is at the end): it opens and has the audio stream.
    AVFormatContext* fmt = nullptr;
    REQUIRE(avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(fmt, nullptr) >= 0);
    CHECK(fmt->nb_streams == 1);
    avformat_close_input(&fmt);
    std::filesystem::remove_all(dir);
}

TEST_CASE("audio streams carry the track names and only the first one is played by default", "[capd]") {
    const auto dir = std::filesystem::temp_directory_path() / ("capd-titles-" + std::to_string(testPid()));
    std::filesystem::create_directories(dir);
    for (const std::string container : {"mkv", "mp4"}) {
        RecordSettings s;
        s.withVideo = false;
        s.container = container;
        s.audio.push_back({0, "master", "Master", "aac", 192});
        s.audio.push_back({3, "track/3", "", "opus", 96});
        const std::string path = (dir / ("rec." + container)).string();
        Recorder rec(s, path, ".capd-titles-none");
        rec.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        REQUIRE(rec.stop().error.empty());

        AVFormatContext* fmt = nullptr;
        REQUIRE(avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) == 0);
        REQUIRE(fmt->nb_streams == 2);
        const char* key = container == "mp4" ? "handler_name" : "title";
        const AVDictionaryEntry* t0 = av_dict_get(fmt->streams[0]->metadata, key, nullptr, 0);
        const AVDictionaryEntry* t1 = av_dict_get(fmt->streams[1]->metadata, key, nullptr, 0);
        REQUIRE(t0);
        REQUIRE(t1);
        CHECK(std::string(t0->value) == "Master");
        CHECK(std::string(t1->value) == "track/3");
        CHECK((fmt->streams[0]->disposition & AV_DISPOSITION_DEFAULT) != 0);
        CHECK((fmt->streams[1]->disposition & AV_DISPOSITION_DEFAULT) == 0);
        avformat_close_input(&fmt);
    }
    std::filesystem::remove_all(dir);
}

// --- A/V sync check ---------------------------------------------------------------------------

TEST_CASE("avsync: flashes, bursts and their pairing", "[capd][avsync]") {
    std::vector<double> t;
    std::vector<float> b;
    for (int i = 0; i < 300; ++i) {
        t.push_back(i / 60.0);
        b.push_back(i % 120 >= 60 && i % 120 < 66 ? 0.9f : 0.05f); // white for 6 frames at 1 s, 3 s
    }
    const auto flashes = findFlashes(t, b);
    REQUIRE(flashes.size() == 2);
    CHECK(flashes[0] == Catch::Approx(1.0));
    CHECK(flashes[1] == Catch::Approx(3.0));
    CHECK(findFlashes(t, std::vector<float>(300, 0.5f)).empty()); // no contrast, no flashes

    std::vector<float> a(48000 * 5, 0.0f);
    for (double start : {0.0, 1.02, 3.03}) // the burst cut by the start of the file is ignored
        for (int i = 0; i < 2400; ++i)
            a[static_cast<std::size_t>(start * 48000) + static_cast<std::size_t>(i)] =
                0.1f * static_cast<float>(std::sin(2 * 3.14159265 * 1000 * i / 48000.0));
    const auto pulses = findPulses(a, 10.0, 48000);
    REQUIRE(pulses.size() == 2);
    CHECK(pulses[0] == Catch::Approx(11.02).margin(0.0005));

    const auto pairs = pairEvents({1.0, 3.0, 9.0}, {1.02, 3.03, 5.0}, 0.5);
    REQUIRE(pairs.size() == 2);
    CHECK(pairs[1].offsetMs() == Catch::Approx(30.0));
    const auto st = computeStats(pairs);
    CHECK(st.meanMs == Catch::Approx(25.0));
    CHECK(st.driftMs == Catch::Approx(10.0));
}

namespace {

// Writes an MKV through capd's Muxer and AudioEncoder (the audio path of real recordings, AAC encoder
// delay included) with MPEG-4 part 2 video at 30 fps: the top-left corner is white from each flash
// time for 5 frames, and the audio has a 50 ms 1 kHz burst audioLagMs after each flash.
bool writeSyncFile(const std::string& path, const std::vector<double>& flashTimes, double audioLagMs, double seconds,
                   const std::string& audioCodec) {
    av_log_set_level(AV_LOG_ERROR);
    const AVCodec* vc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!vc) return false;
    Muxer mux(path, "mkv");
    AVCodecContext* venc = avcodec_alloc_context3(vc);
    venc->width = 320;
    venc->height = 180;
    venc->pix_fmt = AV_PIX_FMT_YUV420P;
    venc->time_base = {1, 30};
    venc->framerate = {30, 1};
    venc->gop_size = 30;
    venc->max_b_frames = 2; // decoded frames come out reordered: the analyser sorts by time
    if (mux.needsGlobalHeader()) venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(venc, vc, nullptr) < 0) return false;
    AudioStreamSettings as;
    as.codec = audioCodec;
    AudioEncoder aenc(as, mux.needsGlobalHeader());
    const int vIndex = mux.addStream(venc);
    const int aIndex = mux.addStream(aenc.context());
    aenc.setSink([&](AVPacket* p) { mux.write(aIndex, p); });
    mux.start();

    AVFrame* f = av_frame_alloc();
    f->format = venc->pix_fmt;
    f->width = venc->width;
    f->height = venc->height;
    av_frame_get_buffer(f, 0);
    AVPacket* pkt = av_packet_alloc();
    auto drainVideo = [&] {
        while (avcodec_receive_packet(venc, pkt) == 0) mux.write(vIndex, pkt);
    };
    const int frames = static_cast<int>(seconds * 30);
    std::vector<float> pcm(1600 * 2);
    for (int i = 0; i < frames; ++i) {
        av_frame_make_writable(f);
        bool white = false;
        for (double ft : flashTimes)
            white |= i >= static_cast<int>(std::lround(ft * 30)) && i < static_cast<int>(std::lround(ft * 30)) + 5;
        for (int y = 0; y < f->height; ++y)
            for (int x = 0; x < f->width; ++x)
                f->data[0][y * f->linesize[0] + x] = static_cast<uint8_t>(white && x < 80 && y < 45 ? 235 : 16);
        for (int p = 1; p <= 2; ++p)
            for (int y = 0; y < f->height / 2; ++y)
                std::memset(f->data[p] + y * f->linesize[p], 128, static_cast<size_t>(f->width / 2));
        f->pts = i;
        avcodec_send_frame(venc, f);
        drainVideo();

        // one video frame's worth of audio (1600 samples) per loop
        for (int k = 0; k < 1600; ++k) {
            const double t = (i * 1600 + k) / 48000.0;
            float v = 0;
            for (double ft : flashTimes) {
                const double d = t - (ft + audioLagMs / 1000.0);
                if (d >= 0 && d < 0.05) v = 0.1f * static_cast<float>(std::sin(2 * 3.14159265 * 1000 * d));
            }
            pcm[static_cast<size_t>(2 * k)] = pcm[static_cast<size_t>(2 * k + 1)] = v;
        }
        aenc.push(pcm, 1600);
    }
    avcodec_send_frame(venc, nullptr);
    drainVideo();
    aenc.finish();
    mux.finish();
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&venc);
    return !mux.failed();
}

} // namespace

TEST_CASE("avsync reads the offset from a recording", "[capd][avsync]") {
    const std::string codec = GENERATE(std::string("aac"), std::string("pcm"));
    CAPTURE(codec);
    const auto path = (std::filesystem::temp_directory_path() / ("dc_avsync_" + std::to_string(testPid()) + ".mkv")).string();
    if (!writeSyncFile(path, {1.0, 3.0, 5.0, 7.0}, 40.0, 9.0, codec)) {
        WARN("MPEG-4 encoder or Matroska muxer missing in this FFmpeg; skipped");
        return;
    }
    AvSyncOptions o;
    o.regionX = 0.05;
    o.regionY = 0.05;
    o.regionW = 0.1;
    o.regionH = 0.1;
    const auto r = analyzeAvSync(path, o);
    if (!std::getenv("DC_KEEP")) std::filesystem::remove(path);
    REQUIRE(r.error.empty());
    CHECK(r.videoFrames == 270);
    CHECK(r.flashes.size() == 4);
    CHECK(r.pulses.size() == 4);
    REQUIRE(r.stats.pairs == 4);
    CHECK(r.stats.meanMs == Catch::Approx(40.0).margin(1.0));
    CHECK(std::fabs(r.stats.driftMs) < 1.0);
}

TEST_CASE("levels: tone and level of a block", "[capd][levels]") {
    std::vector<float> s(48000);
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = 0.1f * static_cast<float>(std::sin(2 * 3.14159265 * 700 * double(i) / 48000.0));
    CHECK(toneLevelDb(s.data(), s.size(), 48000, 700) == Catch::Approx(-20.0).margin(0.1));
    CHECK(toneLevelDb(s.data(), s.size(), 48000, 1000) < -60);
    CHECK(rmsDb(s.data(), s.size()) == Catch::Approx(-23.0).margin(0.1));
    std::vector<float> zero(100, 0.0f);
    CHECK(rmsDb(zero.data(), zero.size()) == -200);
}

TEST_CASE("levels: each audio stream of a recording on its own", "[capd][levels]") {
    av_log_set_level(AV_LOG_ERROR);
    const auto path = (std::filesystem::temp_directory_path() / ("dc_levels_" + std::to_string(testPid()) + ".mkv")).string();
    {
        Muxer mux(path, "mkv");
        AudioStreamSettings as;
        as.codec = "pcm";
        AudioEncoder tone(as, mux.needsGlobalHeader());
        AudioEncoder quiet(as, mux.needsGlobalHeader());
        const int a = mux.addStream(tone.context(), "Master");
        const int b = mux.addStream(quiet.context(), "Mic");
        tone.setSink([&](AVPacket* p) { mux.write(a, p); });
        quiet.setSink([&](AVPacket* p) { mux.write(b, p); });
        mux.start();
        std::vector<float> t(4800 * 2), q(4800 * 2);
        for (int block = 0; block < 50; ++block) { // 5 s
            for (int k = 0; k < 4800; ++k) {
                const double x = (block * 4800 + k) / 48000.0;
                t[static_cast<size_t>(2 * k)] = t[static_cast<size_t>(2 * k + 1)] =
                    0.1f * static_cast<float>(std::sin(2 * 3.14159265 * 700 * x));
                // the second stream: silent for the first 2 s, then a quiet 300 Hz hum
                q[static_cast<size_t>(2 * k)] = q[static_cast<size_t>(2 * k + 1)] =
                    x < 2.0 ? 0.0f : 0.01f * static_cast<float>(std::sin(2 * 3.14159265 * 300 * x));
            }
            tone.push(t, 4800);
            quiet.push(q, 4800);
        }
        tone.finish();
        quiet.finish();
        mux.finish();
        REQUIRE_FALSE(mux.failed());
    }
    std::string error;
    const auto streams = analyzeAudioStreams(path, 700, &error);
    if (!std::getenv("DC_KEEP")) std::filesystem::remove(path);
    REQUIRE(error.empty());
    REQUIRE(streams.size() == 2);
    CHECK(streams[0].title == "Master");
    CHECK(streams[0].seconds == Catch::Approx(5.0).margin(0.05));
    CHECK(streams[0].toneDb == Catch::Approx(-20.0).margin(0.5));
    CHECK(streams[0].toneBlocks == 5);
    CHECK(streams[1].title == "Mic");
    CHECK(streams[1].toneBlocks == 0);
    CHECK(streams[1].silentBlocks == 2);
    CHECK(streams[1].toneDb < -60);
}
