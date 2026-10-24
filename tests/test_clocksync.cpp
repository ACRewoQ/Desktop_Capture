#include "dc/clocksync/audio_fifo.hpp"
#include "dc/clocksync/clock_bridge.hpp"
#include "dc/clocksync/drift_sim.hpp"
#include "dc/clocksync/rate_converter.hpp"
#include "dc/clocksync/resampler.hpp"
#include "dc/clocksync/time_dll.hpp"
#include "dc/clocksync/tone_analyzer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <vector>

using namespace dc::clocksync;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(std::size_t frames, double hz, double amp = 0.5, double rate = 48000) {
    std::vector<float> v(frames);
    for (std::size_t i = 0; i < frames; ++i) v[i] = float(amp * std::sin(2 * kPi * hz * double(i) / rate));
    return v;
}

// Runs a mono signal through the resampler in blocks with a fixed ratio.
std::vector<float> resample(const std::vector<float>& in, double ratio, std::size_t block) {
    VariableResampler r(1);
    std::vector<float> out;
    std::size_t used = 0;
    std::vector<float> tmp(block);
    while (true) {
        const auto need = r.inputFramesRequired(block, ratio);
        if (used + need > in.size()) break;
        std::copy_n(in.data() + used, need, r.inputWritePointer(need));
        r.commitInput(need);
        used += need;
        r.produce(tmp.data(), block, ratio);
        out.insert(out.end(), tmp.begin(), tmp.end());
    }
    return out;
}

} // namespace

TEST_CASE("audio fifo keeps order across the wrap point", "[clocksync]") {
    AudioFifo f(2, 100); // rounds up to 128
    CHECK(f.capacity() == 128);
    std::vector<float> in(2 * 90), out(2 * 90);
    float next = 0, expect = 0;
    for (int round = 0; round < 10; ++round) {
        for (auto& s : in) s = next++;
        CHECK(f.push(in.data(), 90) == 90);
        CHECK(f.push(in.data(), 90) == 38); // full
        next -= 2 * 52;                     // those 52 frames were dropped; resend them next round
        CHECK(f.pop(out.data(), 90) == 90);
        for (std::size_t i = 0; i < 180; ++i) REQUIRE(out[i] == expect++);
        f.skip(f.available());
        expect = next;
    }
}

TEST_CASE("resampler at ratio 1 passes a tone cleanly with constant delay", "[clocksync]") {
    const auto in = sine(48000, 1000);
    const auto out = resample(in, 1.0, 480);
    REQUIRE(out.size() > 40000);
    // output frame k corresponds to input frame k (the kernel is centred on the read position)
    double err = 0;
    for (std::size_t k = 1000; k < 40000; ++k) err = std::max(err, double(std::abs(out[k] - in[k])));
    CHECK(err < 1e-4); // -86 dB of a 0.5 tone: only the passband ripple of the kernel
}

TEST_CASE("resampler changes pitch by the ratio without artefacts", "[clocksync]") {
    const double ratio = 1.0005; // 500 ppm
    const auto in = sine(48000 * 3, 1000);
    const auto out = resample(in, ratio, 441);
    ToneAnalyzer::Config cfg;
    cfg.toneHz = 1000 * ratio;
    ToneAnalyzer a(cfg);
    a.feed(out.data() + 4800, out.size() - 4800, 1);
    REQUIRE(a.summary().blocks >= 20);
    CHECK(a.summary().glitchBlocks == 0);
    CHECK(a.summary().minSinadDb > 90);
}

TEST_CASE("resampler survives ratio changes at every block", "[clocksync]") {
    const auto in = sine(48000 * 3, 997);
    VariableResampler r(1);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> ppm(-1000, 1000);
    std::uniform_int_distribution<std::size_t> len(1, 2000);
    std::vector<float> out, tmp(4096);
    std::size_t used = 0;
    double ratio = 1;
    while (true) {
        ratio = std::clamp(ratio + ppm(rng) * 1e-8, 0.999, 1.001); // a slow random walk
        const auto n = len(rng);
        const auto need = r.inputFramesRequired(n, ratio);
        if (used + need > in.size()) break;
        std::copy_n(in.data() + used, need, r.inputWritePointer(need));
        r.commitInput(need);
        used += need;
        r.produce(tmp.data(), n, ratio);
        out.insert(out.end(), tmp.begin(), tmp.begin() + std::ptrdiff_t(n));
        CHECK(r.bufferedFrames() >= 0);
    }
    ToneAnalyzer a({48000, 997, 4800, 60, 0.01, 10});
    a.feed(out.data() + 4800, out.size() - 4800, 1);
    CHECK(a.summary().glitchBlocks == 0);
}

TEST_CASE("tone analyzer flags a dropped sample and a click", "[clocksync]") {
    auto s = sine(48000, 1000);
    ToneAnalyzer clean;
    clean.feed(s.data(), s.size(), 1);
    CHECK(clean.summary().blocks == 10);
    CHECK(clean.summary().glitchBlocks == 0);
    CHECK(clean.summary().minSinadDb > 100);

    auto dropped = s;
    dropped.erase(dropped.begin() + 12345);
    ToneAnalyzer a;
    a.feed(dropped.data(), dropped.size(), 1);
    CHECK(a.summary().glitchBlocks >= 1);
    CHECK(a.summary().events.front().index == 2);

    auto click = s;
    click[30000] += 0.05f; // one-sample spike, -20 dB below the tone
    ToneAnalyzer b;
    b.feed(click.data(), click.size(), 1);
    CHECK(b.summary().glitchBlocks == 1);
}

TEST_CASE("time DLL locks to the device rate through jittery timestamps", "[clocksync]") {
    TimeDll dll(48000, 0.1);
    std::mt19937 rng(3);
    std::normal_distribution<double> jitter(0, 0.0001); // 100 us
    const double rate = 48000 * (1 + 150e-6);
    for (std::uint64_t k = 0; k < 6000; ++k) dll.update(k * 480, 100.0 + double(k * 480) / rate + jitter(rng));
    CHECK(dll.ready());
    CHECK((dll.rate() / rate - 1) * 1e6 == Catch::Approx(0).margin(10));
    const double t = 100.0 + 6000.0 * 480 / rate;
    CHECK(dll.positionAt(t) == Catch::Approx(6000.0 * 480).margin(3));
}

TEST_CASE("bridge restarts cleanly after a producer discontinuity", "[clocksync]") {
    ClockBridge::Config cfg;
    cfg.channels = 1;
    ClockBridge b(cfg);
    std::vector<float> in(480, 0.25f), out(480);
    double t = 0;
    for (int i = 0; i < 300; ++i, t += 0.01) {
        b.push(in.data(), 480, t);
        b.pull(out.data(), 480, t + 0.005);
        if (i == 150) b.producerDiscontinuity();
    }
    const auto s = b.stats();
    CHECK(s.state == ClockBridge::State::Running);
    CHECK(s.resyncs == 1);
    CHECK(s.underruns == 0);
    CHECK(s.overruns == 0);
    CHECK(out[100] == Catch::Approx(0.25f).margin(1e-3));
}

TEST_CASE("bridge recovers from a consumer stall without winding up", "[clocksync]") {
    // G2: the clock device was reopened while an input kept streaming; the backlog was drained at the
    // ratio limit for a minute and the wound-up controller then ran the FIFO dry
    ClockBridge::Config cfg;
    cfg.channels = 1;
    cfg.targetFrames = 1344;
    ClockBridge b(cfg);
    std::vector<float> in(480, 0.25f), out(480);
    double t = 0;
    for (int i = 0; i < 12000; ++i, t += 0.01) {
        b.push(in.data(), 480, t);
        if (i >= 300 && i < 315) continue; // the consumer stalls for 150 ms
        b.pull(out.data(), 480, t + 0.005);
    }
    const auto s = b.stats();
    CHECK(s.state == ClockBridge::State::Running);
    CHECK(s.underruns == 0);
    CHECK(s.overruns == 0);
    CHECK(s.fifoFrames < 2.0 * cfg.targetFrames);
    CHECK(std::abs(s.ratio - 1.0) < 1e-4);
}

TEST_CASE("simulated devices stay locked without glitches", "[clocksync][sim]") {
    SimConfig s;
    s.seconds = 150;
    s.warmupSeconds = 60;
    s.bridge.channels = 1;
    SECTION("typical +-50 ppm") {
        s.producer = {+40, 0, 600, 480, 0.002, 20e-6};
        s.consumer = {-25, 0, 600, 480, 0.002, 20e-6};
    }
    SECTION("1000 ppm apart, different periods") {
        s.producer = {+500, 0, 600, 480, 0.002, 20e-6};
        s.consumer = {-500, 0, 600, 144, 0.001, 20e-6};
    }
    const auto r = runSimulation(s);
    INFO(r.verdict());
    CHECK(r.passed());
    CHECK(r.estimatedPpm == Catch::Approx(r.truePpm).margin(2));
    CHECK(r.latencyDriftMs < 0.1);
}

TEST_CASE("rate converter: 44.1 kHz capture to 48 kHz in irregular packets", "[clocksync]") {
    const auto in = sine(44100 * 3, 1000, 0.5, 44100);
    RateConverter c(1, 44100, 48000, 2048);
    std::mt19937 rng(3);
    std::uniform_int_distribution<std::size_t> len(1, 1500);
    std::vector<float> out;
    std::vector<double> stamps;
    std::size_t used = 0;
    double t = 10.0;
    while (used < in.size()) {
        const auto n = std::min(len(rng), in.size() - used);
        c.push(in.data() + used, n, t, [&](const float* p, std::size_t k, double time) {
            // the time of output frame j is time + j / 48000; it must match the input's own clock
            stamps.push_back(time - double(out.size()) / 48000);
            out.insert(out.end(), p, p + k);
        });
        used += n;
        t += double(n) / 44100;
    }
    CHECK(static_cast<double>(out.size()) == Catch::Approx(static_cast<double>(in.size()) * 48000 / 44100).margin(80));
    ToneAnalyzer a({48000, 1000, 4800, 60, 0.01, 10});
    a.feed(out.data() + 4800, out.size() - 9600, 1);
    REQUIRE(a.summary().blocks >= 20);
    CHECK(a.summary().glitchBlocks == 0);
    CHECK(a.summary().minSinadDb > 85);
    // every block's stamp describes the same timeline (within a frame)
    for (double s : stamps) CHECK(s == Catch::Approx(stamps.front()).margin(1.0 / 44100));
}

TEST_CASE("rate converter: 48 kHz engine to a 44.1 kHz device, pulled in device periods", "[clocksync]") {
    const auto in = sine(48000 * 3, 1000);
    RateConverter c(2, 48000, 44100, 1024);
    std::size_t used = 0;
    std::vector<float> out, block(441 * 2 * 4);
    std::mt19937 rng(5);
    std::uniform_int_distribution<std::size_t> len(64, 441 * 4);
    while (true) {
        const auto n = len(rng);
        bool ran_out = false;
        c.pull(block.data(), n, [&](float* dst, std::size_t k) {
            for (std::size_t i = 0; i < k; ++i) {
                const float v = used < in.size() ? in[used] : 0.0f;
                ran_out = ran_out || used >= in.size();
                dst[2 * i] = v;
                dst[2 * i + 1] = -v;
                ++used;
            }
        });
        if (ran_out) break;
        out.insert(out.end(), block.begin(), block.begin() + std::ptrdiff_t(2 * n));
    }
    std::vector<float> left(out.size() / 2), right(out.size() / 2);
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = out[2 * i];
        right[i] = out[2 * i + 1];
    }
    CHECK(double(left.size()) == Catch::Approx(double(used) * 44100 / 48000).margin(1500));
    ToneAnalyzer a({44100, 1000, 4410, 60, 0.01, 10});
    a.feed(left.data() + 4410, left.size() - 4410, 1);
    REQUIRE(a.summary().blocks >= 20);
    CHECK(a.summary().glitchBlocks == 0);
    CHECK(a.summary().minSinadDb > 85);
    CHECK(right[20000] == Catch::Approx(-left[20000]).margin(1e-6));
}

TEST_CASE("rate converter: going down in rate filters what would fold back", "[clocksync]") {
    const auto in = sine(48000, 23000); // above the 22.05 kHz Nyquist of 44.1 kHz
    RateConverter c(1, 48000, 44100, 1024);
    std::size_t used = 0;
    std::vector<float> out(44100 / 2);
    c.pull(out.data(), out.size(), [&](float* dst, std::size_t k) {
        for (std::size_t i = 0; i < k; ++i) dst[i] = in[used++];
    });
    float peak = 0;
    for (std::size_t i = 2000; i < out.size(); ++i) peak = std::max(peak, std::fabs(out[i]));
    CHECK(peak < 0.5f * 0.001f); // below -60 dB of the tone
}
