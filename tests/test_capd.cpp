// capd's portable parts: settings from the parameter tree, and the audio time line (shared memory -> recording).

#include "audio_capture.hpp"
#include "settings.hpp"

#include "dc/protocol/defaults.hpp"
#include "dc/shmaudio/clock.hpp"
#include "dc/shmaudio/ring.hpp"
#include "dc/shmaudio/shared_memory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
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
                baseQpc + static_cast<std::int64_t>(static_cast<double>(frame) * double(f) / shm::kSampleRate);
            if (blockQpc + f * kBlock / shm::kSampleRate > qpc) return;
            std::vector<float> st(kBlock * 2), mono(kBlock);
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                st[i * 2] = st[i * 2 + 1] = static_cast<float>(frame + i);
                mono[i] = -static_cast<float>(frame + i);
            }
            writer.write(0, st.data(), kBlock, 2);
            writer.write(1, mono.data(), kBlock, 1);
            writer.commit(kBlock, blockQpc);
        }
    }
    static constexpr std::uint32_t kBlock = 480;
    shm::AudioRingWriter writer;
    std::int64_t baseQpc = 0;
};

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
    CHECK(std::fabs(out.master[0] - 24000.0f) <= 1.0f);
    for (std::size_t i = 1; i < out.master.size(); ++i) REQUIRE(out.master[i] == out.master[i - 1] + 1.0f);
    CHECK(out.track[100] == -out.master[100]);
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
