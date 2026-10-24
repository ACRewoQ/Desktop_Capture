#include "dc/clocksync/tone_analyzer.hpp"
#include "dc/lanmedia/arrival_clock.hpp"
#include "dc/lanmedia/audio_codec.hpp"
#include "dc/lanmedia/audio_receiver.hpp"
#include "dc/lanmedia/audio_sender.hpp"
#include "dc/lanmedia/net_sim.hpp"
#include "dc/lanmedia/packet.hpp"
#include "dc/lanmedia/udp_transport.hpp"
#include "dc/lanmedia/video.hpp"
#include "dc/lanmedia/video_link.hpp"
#include "dc/lanmedia/video_sim.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace dc::lan;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> stereoSine(std::size_t frames, std::size_t start, double hz = 997, double amp = 0.5) {
    std::vector<float> v(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const double ph = 2 * kPi * hz * double(start + i) / 48000.0;
        v[2 * i] = float(amp * std::sin(ph));
        v[2 * i + 1] = float(amp * std::sin(ph + 0.5));
    }
    return v;
}

using Datagram = std::vector<std::uint8_t>;

// Sends `seconds` of a tone through a sender in 480-frame blocks and returns the datagrams in order.
std::vector<Datagram> makeStream(AudioCodec codec, std::size_t packetFrames, double seconds, int channels = 2) {
    AudioSender::Config c;
    c.encoder.codec = codec;
    c.encoder.frames = packetFrames;
    c.encoder.channels = channels;
    AudioSender s(c);
    std::vector<Datagram> out;
    const std::size_t blocks = std::size_t(seconds * 100);
    for (std::size_t b = 0; b < blocks; ++b) {
        auto block = stereoSine(480, b * 480);
        if (channels == 1)
            for (std::size_t i = 0; i < 480; ++i) block[i] = block[2 * i];
        s.push(block.data(), 480, double(b) * 0.01);
        s.pump([&](std::span<const std::uint8_t> d) { out.emplace_back(d.begin(), d.end()); });
    }
    return out;
}

struct Arrival {
    double time;
    Datagram data;
};

// Arrivals for packets sent at their nominal time (end of their data) plus delay[i]; < 0 = lost.
std::vector<Arrival> arrivals(const std::vector<Datagram>& packets, std::size_t packetFrames,
                              const std::vector<double>& delay) {
    std::vector<Arrival> a;
    for (std::size_t i = 0; i < packets.size(); ++i)
        if (delay[i] >= 0) a.push_back({double((i + 1) * packetFrames) / 48000.0 + delay[i], packets[i]});
    return a;
}

// Plays arrivals into a receiver; pulls 128-frame blocks on the same clock. Returns the output (stereo).
std::vector<float> play(AudioReceiver& r, std::vector<Arrival> arr, double seconds) {
    std::stable_sort(arr.begin(), arr.end(), [](const Arrival& a, const Arrival& b) { return a.time < b.time; });
    std::vector<float> out;
    std::vector<float> block(128 * 2);
    std::size_t next = 0;
    for (std::size_t k = 0; double(k * 128) / 48000.0 < seconds; ++k) {
        const double t = double(k * 128) / 48000.0;
        for (; next < arr.size() && arr[next].time <= t; ++next) r.onPacket(arr[next].data, arr[next].time);
        r.pull(block.data(), 128, t);
        out.insert(out.end(), block.begin(), block.end());
    }
    return out;
}

dc::clocksync::ToneAnalyzer::Summary analyze(const std::vector<float>& stereo, double skipSeconds, double sinad = 60) {
    dc::clocksync::ToneAnalyzer::Config c;
    c.toneHz = 997;
    c.glitchSinadDb = sinad;
    dc::clocksync::ToneAnalyzer a(c);
    const std::size_t skip = std::size_t(skipSeconds * 48000) * 2;
    a.feed(stereo.data() + skip, (stereo.size() - skip) / 2, 2);
    return a.summary();
}

} // namespace

TEST_CASE("lanmedia: media header and audio payload round trip", "[lanmedia]") {
    MediaHeader h;
    h.kind = MediaKind::Audio;
    h.flags = kFlagStreamStart;
    h.streamId = 0xDEADBEEF;
    h.seq = 0xFFFFFFFE;
    h.timeUs = 123456789012345ull;
    h.position = (1ull << 40) + 7;
    std::vector<std::uint8_t> d(kMediaHeaderBytes + kAudioHeaderBytes + 120 * 2 * 3);
    writeMediaHeader(d.data(), h);
    writeAudioHeader(d.data() + kMediaHeaderBytes, {AudioCodec::Pcm24, 2, 120});

    REQUIRE(peekType(d) == PacketType::Media);
    const auto m = parseMedia(d);
    REQUIRE(m);
    CHECK(m->header == h);
    const auto a = parseAudio(m->payload);
    REQUIRE(a);
    CHECK(a->header == AudioHeader{AudioCodec::Pcm24, 2, 120});
    CHECK(a->data.size() == 720);

    // a PCM payload of the wrong size, a bad magic, a truncated header are rejected
    auto shortPcm = d;
    shortPcm.pop_back();
    CHECK_FALSE(parseAudio(parseMedia(shortPcm)->payload));
    auto badMagic = d;
    badMagic[0] = 'X';
    CHECK_FALSE(parseMedia(badMagic));
    CHECK_FALSE(parseMedia(std::span<const std::uint8_t>(d.data(), kMediaHeaderBytes - 1)));
}

TEST_CASE("lanmedia: report round trip", "[lanmedia]") {
    Report r;
    r.flags = kReportKeyframeRequest;
    r.streamId = 42;
    r.received = 1000;
    r.lost = 3;
    r.late = 2;
    r.concealed = 5;
    r.jitterUs = 1234;
    r.latencyUs = 9876;
    r.echoTimeUs = 555555555555ull;
    r.echoHoldUs = 777;
    const auto d = encodeReport(r);
    CHECK(d.size() == kReportBytes);
    REQUIRE(peekType(d) == PacketType::Report);
    CHECK(parseReport(d) == r);
    CHECK_FALSE(parseMedia(d));
}

TEST_CASE("lanmedia: sequence numbers unwrap across 2^32", "[lanmedia]") {
    SeqUnwrapper u;
    const auto a = u.unwrap(0xFFFFFFFD);
    CHECK(u.unwrap(0xFFFFFFFF) == a + 2);
    CHECK(u.unwrap(1) == a + 4);
    CHECK(u.unwrap(0xFFFFFFFE) == a + 1); // late packet from before the wrap
    CHECK(u.unwrap(2) == a + 5);
}

TEST_CASE("lanmedia: PCM codecs round trip", "[lanmedia]") {
    for (const auto codec : {AudioCodec::Pcm16, AudioCodec::Pcm24}) {
        AudioEncoder enc({codec, 2, 120});
        AudioDecoder dec(codec, 2, 120);
        auto in = stereoSine(120, 0, 997, 0.9);
        in[0] = 1.0f; // full scale must not wrap
        in[1] = -1.0f;
        std::vector<std::uint8_t> buf(kMaxMediaPayloadBytes);
        const auto n = enc.encode(in.data(), buf.data(), buf.size());
        CHECK(n == pcmBytes(codec, 2, 120));
        std::vector<float> out(240);
        REQUIRE(dec.decode({buf.data(), n}, out.data()));
        const double tol = codec == AudioCodec::Pcm16 ? 1.0 / 32768 : 1.0 / 8388608;
        for (std::size_t i = 0; i < in.size(); ++i) CHECK(std::abs(out[i] - in[i]) <= tol * 1.01);
    }
    CHECK(validPacketFrames(AudioCodec::Pcm24, 2, 120));
    CHECK_FALSE(validPacketFrames(AudioCodec::Pcm24, 2, 300)); // 1800 bytes: does not fit a datagram
    CHECK_FALSE(validPacketFrames(AudioCodec::Opus, 2, 128));
}

TEST_CASE("lanmedia: Opus encodes and decodes a tone, FEC and PLC keep the timeline", "[lanmedia]") {
    AudioEncoder enc({AudioCodec::Opus, 2, 480, 128000, true, 10});
    AudioDecoder dec(AudioCodec::Opus, 2, 480);
    std::vector<std::vector<std::uint8_t>> packets;
    for (int p = 0; p < 50; ++p) {
        auto in = stereoSine(480, std::size_t(p) * 480);
        std::vector<std::uint8_t> buf(1400);
        const auto n = enc.encode(in.data(), buf.data(), buf.size());
        REQUIRE(n > 0);
        REQUIRE(n < 1400);
        buf.resize(n);
        packets.push_back(buf);
    }
    std::vector<float> out(960), all;
    for (int p = 0; p < 50; ++p) {
        if (p == 30) {
            CHECK(dec.decodeFec(packets[31], out.data())); // packet 30 lost, rebuilt from 31
        } else if (p == 40) {
            CHECK(dec.conceal(out.data()));
        } else {
            REQUIRE(dec.decode(packets[std::size_t(p)], out.data()));
        }
        all.insert(all.end(), out.begin(), out.end());
    }
    // after the codec's start-up the tone comes through at the right level
    float peak = 0;
    for (std::size_t i = 48000 / 2 * 2 / 10; i < 20 * 960; ++i) peak = std::max(peak, std::abs(all[i]));
    CHECK(peak == Catch::Approx(0.5).margin(0.05));
}

TEST_CASE("lanmedia: arrival clock follows the fastest path and measures lateness", "[lanmedia]") {
    ArrivalClock c;
    std::mt19937 rng(3);
    std::exponential_distribution<double> expo(1.0 / 0.002); // 2 ms mean extra delay
    double maxLate = 0;
    for (int i = 1; i <= 2000; ++i) {
        const std::uint64_t end = std::uint64_t(i) * 120;
        const double sent = double(end) / 48000.0;
        const double extra = (i % 500 == 0) ? 0.0 : expo(rng); // now and then a packet takes the fastest path
        maxLate = std::max(maxLate, c.update(end, 10.0 + sent + 0.0003 + extra));
    }
    // the envelope sits on the fastest path (0.3 ms), not on the mean
    const double t = 10.0 + 2000 * 120 / 48000.0;
    CHECK(c.availableAt(t + 0.0003) / 48000.0 == Catch::Approx(2000 * 120 / 48000.0).margin(0.0002));
    CHECK(c.latenessPeak(t) > 0.005);
    CHECK(c.latenessPeak(t) <= maxLate + 1e-9);
}

TEST_CASE("lanmedia: receiver plays an in-order stream without clicks at a small latency", "[lanmedia]") {
    const auto packets = makeStream(AudioCodec::Pcm24, 120, 6);
    AudioReceiver r({});
    const auto out = play(r, arrivals(packets, 120, std::vector<double>(packets.size(), 0.0005)), 6);
    const auto s = r.stats();
    CHECK(s.state == AudioReceiver::State::Running);
    CHECK(s.concealed == 0);
    CHECK(s.late == 0);
    CHECK(s.resyncs == 0);
    CHECK(s.targetMs < 8);
    const auto tone = analyze(out, 1.0);
    CHECK(tone.blocks > 40);
    CHECK(tone.glitchBlocks == 0);
}

TEST_CASE("lanmedia: receiver reorders, drops duplicates and conceals lost packets", "[lanmedia]") {
    const auto packets = makeStream(AudioCodec::Pcm24, 120, 6);
    std::vector<double> delay(packets.size());
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> uni(0.0003, 0.004); // up to 1.5 packets of reordering
    for (auto& d : delay) d = uni(rng);
    delay[1000] = -1; // lost
    delay[1500] = -1;
    delay[1501] = -1;
    auto arr = arrivals(packets, 120, delay);
    arr.push_back({double(1201 * 120) / 48000.0 + delay[1200] + 0.001, packets[1200]}); // a duplicate, later
    AudioReceiver r({});
    const auto out = play(r, arr, 6);
    const auto s = r.stats();
    CHECK(s.duplicates == 1);
    CHECK(s.lost == 3);
    CHECK(s.concealed == 3);
    CHECK(s.late == 0);
    CHECK(s.resyncs == 0);
    // the two concealments are the only disturbances
    const auto tone = analyze(out, 1.0);
    CHECK(tone.glitchBlocks >= 1);
    CHECK(tone.glitchBlocks <= 3);
}

TEST_CASE("lanmedia: a mono stream plays on both channels", "[lanmedia]") {
    const auto packets = makeStream(AudioCodec::Pcm16, 240, 2, 1);
    AudioReceiver r({});
    const auto out = play(r, arrivals(packets, 240, std::vector<double>(packets.size(), 0.001)), 2);
    REQUIRE(r.stats().streamChannels == 1);
    float diff = 0, peak = 0;
    for (std::size_t i = out.size() / 2; i + 1 < out.size(); i += 2) {
        diff = std::max(diff, std::abs(out[i] - out[i + 1]));
        peak = std::max(peak, std::abs(out[i]));
    }
    CHECK(peak > 0.4f);
    CHECK(diff == 0.0f);
}

TEST_CASE("lanmedia: receiver keeps its stream and switches only on a stream start", "[lanmedia]") {
    const auto a = makeStream(AudioCodec::Pcm24, 120, 1);
    const auto b = makeStream(AudioCodec::Pcm24, 120, 1);
    AudioReceiver r({});
    std::vector<float> block(256);
    r.onPacket(a[0], 0.001);
    r.pull(block.data(), 128, 0.002);
    const auto first = r.stats().streamId;
    r.onPacket(b[5], 0.003); // another sender's packet in the middle of its stream: ignored
    r.pull(block.data(), 128, 0.004);
    CHECK(r.stats().streamId == first);
    CHECK(r.stats().rejected == 1);
    r.onPacket(b[0], 0.005); // its first packet carries StreamStart: switch
    r.pull(block.data(), 128, 0.006);
    CHECK(r.stats().streamId != first);
    CHECK(r.stats().streamChanges == 1);
}

TEST_CASE("lanmedia: sender reports round trip time from the receiver's echo", "[lanmedia]") {
    AudioSender s({});
    Report rep;
    rep.streamId = s.streamId();
    rep.echoTimeUs = 1'000'000;
    rep.echoHoldUs = 2'000;
    s.onReport(rep, 1.005);
    const auto st = s.stats();
    REQUIRE(st.haveReport);
    CHECK(st.rttMs == Catch::Approx(3.0));
    rep.streamId = s.streamId() + 1; // not ours
    rep.echoHoldUs = 0;
    s.onReport(rep, 2.0);
    CHECK(s.stats().rttMs == Catch::Approx(3.0));
}

TEST_CASE("lanmedia: network simulation scenarios (shortened)", "[lanmedia][sim]") {
    for (auto s : standardScenarios()) {
        INFO(s.name);
        s.seconds = 60;
        s.warmupSeconds = 15;
        for (auto& e : s.events) e.atSec = 30;
        // short runs: the concealment share of an event is larger than in the 10 minute runs
        s.expect.maxConcealedPercent *= 10;
        // and the Wi-Fi target moves (spikes) weigh more in the mean ratio
        if (s.expect.maxDriftErrorPpm > 20) s.expect.maxDriftErrorPpm = 500;
        const auto r = runSimulation(s);
        INFO(r.verdict());
        CHECK(r.passed());
    }
}

namespace {

// One access unit of `bytes` bytes whose content identifies frame n.
Datagram accessUnit(std::uint64_t n, std::size_t bytes) {
    Datagram au(bytes);
    for (std::size_t i = 0; i < bytes; ++i) au[i] = std::uint8_t((n * 31 + i) & 0xFF);
    return au;
}

struct VideoCapture {
    std::vector<std::uint64_t> numbers;
    std::vector<bool> keys;
    bool contentOk = true;
    VideoReceiver::FrameHandler handler() {
        return [this](const VideoReceiver::Frame& f) {
            numbers.push_back(f.number);
            keys.push_back(f.keyframe);
            const auto want = accessUnit(f.number, f.data.size());
            if (!std::equal(want.begin(), want.end(), f.data.begin())) contentOk = false;
        };
    }
};

// Frames of a stream as datagram lists, frame i a keyframe when key(i).
std::vector<std::vector<Datagram>> videoStream(VideoSender& sender, int frames, std::size_t bytes,
                                               const std::function<bool(int)>& key) {
    std::vector<std::vector<Datagram>> out;
    for (int i = 0; i < frames; ++i) {
        std::vector<Datagram> d;
        sender.send(accessUnit(std::uint64_t(i), bytes), key(i), i / 30.0, i / 30.0,
                    [&](std::span<const std::uint8_t> x) { d.emplace_back(x.begin(), x.end()); });
        out.push_back(std::move(d));
    }
    return out;
}

} // namespace

TEST_CASE("lanmedia: video header round trip and fragment bounds", "[lanmedia][video]") {
    std::uint8_t buf[kMaxDatagramBytes]{};
    MediaHeader h;
    h.kind = MediaKind::Video;
    h.flags = kFlagKeyframe;
    h.streamId = 77;
    h.position = 12;
    h.fragIndex = 1;
    h.fragCount = 3;
    writeMediaHeader(buf, h);
    writeVideoHeader(buf + kMediaHeaderBytes, {VideoCodec::H264, 3000, 1352});
    const std::size_t n = kMediaHeaderBytes + kVideoHeaderBytes + 1352;
    const auto m = parseMedia({buf, n});
    REQUIRE(m);
    CHECK(m->header == h);
    const auto v = parseVideo(m->payload);
    REQUIRE(v);
    CHECK(v->header.frameBytes == 3000);
    CHECK(v->header.offset == 1352);
    CHECK(v->data.size() == 1352);
    // a fragment reaching past the end of the frame is rejected
    writeVideoHeader(buf + kMediaHeaderBytes, {VideoCodec::H264, 2000, 1352});
    CHECK_FALSE(parseVideo(parseMedia({buf, n})->payload));
}

TEST_CASE("lanmedia: video frames are fragmented and reassembled in order", "[lanmedia][video]") {
    VideoSender sender;
    auto frames = videoStream(sender, 10, 5000, [](int i) { return i == 0; });
    CHECK(frames[0].size() == 4); // 5000 B / 1352 B per fragment
    for (const auto& d : frames[0]) CHECK(d.size() <= kMaxDatagramBytes);

    VideoReceiver rx;
    VideoCapture cap;
    // fragments of each frame arrive in reverse, frames in order
    double t = 0;
    for (auto& f : frames) {
        for (auto it = f.rbegin(); it != f.rend(); ++it) rx.onPacket(*it, t, cap.handler());
        t += 1 / 30.0;
        rx.poll(t, cap.handler());
    }
    CHECK(cap.numbers == std::vector<std::uint64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    CHECK(cap.contentOk);
    CHECK(cap.keys[0]);
    CHECK(rx.stats().droppedFrames == 0);
    CHECK(rx.stats().keyframeRequests == 0);
}

TEST_CASE("lanmedia: a lost fragment drops frames until a keyframe and asks for one", "[lanmedia][video]") {
    VideoSender sender;
    auto frames = videoStream(sender, 12, 3000, [](int i) { return i == 0 || i == 8; });
    VideoReceiver rx;
    VideoCapture cap;
    bool asked = false;
    double t = 0;
    for (int i = 0; i < 12; ++i) {
        for (std::size_t k = 0; k < frames[std::size_t(i)].size(); ++k)
            if (!(i == 3 && k == 1)) rx.onPacket(frames[std::size_t(i)][k], t, cap.handler());
        for (int p = 0; p < 16; ++p) {
            t += 1 / 30.0 / 16;
            if (auto r = rx.poll(t, cap.handler()); r && (r->flags & kReportKeyframeRequest)) {
                asked = true;
                sender.onReport(*r, t);
            }
        }
    }
    CHECK(asked);
    CHECK(sender.stats().keyframeRequests >= 1);
    // 0..2 decodable, 3 broken, 4..7 depend on it, 8 is the next keyframe
    CHECK(cap.numbers == std::vector<std::uint64_t>{0, 1, 2, 8, 9, 10, 11});
    CHECK(cap.contentOk);
    CHECK(rx.stats().droppedFrames == 5);
}

TEST_CASE("lanmedia: a viewer joining mid-GOP gets a keyframe first", "[lanmedia][video]") {
    VideoSender sender;
    auto frames = videoStream(sender, 20, 2000, [](int i) { return i == 0 || i == 15; });
    VideoReceiver rx;
    VideoCapture cap;
    double t = 0;
    bool asked = false;
    for (int i = 5; i < 20; ++i) {
        for (const auto& d : frames[std::size_t(i)]) rx.onPacket(d, t, cap.handler());
        t += 1 / 30.0;
        if (auto r = rx.poll(t, cap.handler()); r && (r->flags & kReportKeyframeRequest)) asked = true;
    }
    CHECK(asked);
    REQUIRE_FALSE(cap.numbers.empty());
    CHECK(cap.numbers.front() == 15);
    CHECK(cap.keys.front());
}

TEST_CASE("lanmedia: video receiver follows a restarted sender", "[lanmedia][video]") {
    VideoSender sender;
    auto a = videoStream(sender, 3, 2000, [](int i) { return i == 0; });
    sender.restart();
    auto b = videoStream(sender, 3, 2000, [](int i) { return i == 0; });
    VideoReceiver rx;
    VideoCapture cap;
    for (auto* s : {&a, &b})
        for (auto& f : *s)
            for (auto& d : f) rx.onPacket(d, 0, cap.handler());
    CHECK(cap.numbers == std::vector<std::uint64_t>{0, 1, 2, 0, 1, 2});
    CHECK(rx.stats().streamChanges == 1);
}

TEST_CASE("lanmedia: video sender holds a keyframe request it just answered", "[lanmedia][video]") {
    VideoSender sender;
    Report r;
    r.streamId = sender.streamId();
    r.flags = kReportKeyframeRequest;
    sender.send(accessUnit(0, 100), true, 0, 10.0, [](auto) {});
    sender.onReport(r, 10.02); // the keyframe left 20 ms ago
    CHECK_FALSE(sender.keyframeWanted());
    sender.onReport(r, 10.2);
    CHECK(sender.keyframeWanted());
    sender.send(accessUnit(1, 100), true, 0, 10.25, [](auto) {});
    CHECK_FALSE(sender.keyframeWanted());
    r.streamId = 1234; // other streams are ignored
    sender.onReport(r, 11);
    CHECK_FALSE(sender.keyframeWanted());
}

TEST_CASE("lanmedia: video network simulation scenarios (shortened)", "[lanmedia][sim][video]") {
    for (auto s : standardVideoScenarios()) {
        INFO(s.name);
        s.seconds = 30;
        for (auto& e : s.events) e.atSec = std::min(e.atSec, 12.0);
        const auto r = runVideoSimulation(s);
        INFO(r.verdict());
        CHECK(r.passed());
        CHECK(r.undecodable == 0);
    }
}

TEST_CASE("lanmedia: audio over real UDP sockets on loopback", "[lanmedia][udp]") {
    UdpTransport::Options o;
    o.bindAddress = "127.0.0.1";
    o.port = 0;
    UdpTransport rx(o), tx(o);
    AudioReceiver receiver({});
    rx.start([&](std::span<const std::uint8_t> d, const UdpTransport::Endpoint&, double t) { receiver.onPacket(d, t); });
    const auto to = UdpTransport::Endpoint(asio::ip::make_address("127.0.0.1"), rx.localPort());

    AudioSender sender({});
    const auto packets = 400; // 1 s of 2.5 ms packets, sent in 10 ms bursts
    std::vector<float> out(256);
    for (int b = 0; b < packets / 4; ++b) {
        const auto block = stereoSine(480, std::size_t(b) * 480);
        sender.push(block.data(), 480, nowSeconds());
        sender.pump([&](std::span<const std::uint8_t> d) { tx.send(d, to); });
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        receiver.pull(out.data(), 128, nowSeconds());
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (rx.received() < std::uint64_t(packets) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    rx.stop();
    receiver.pull(out.data(), 128, nowSeconds()); // drains the queue
    CHECK(rx.received() == std::uint64_t(packets));
    const auto s = receiver.stats();
    CHECK(s.received == std::uint64_t(packets));
    CHECK(s.lost == 0);
    CHECK(s.rejected == 0);
    CHECK(s.streamId == sender.streamId());
}

TEST_CASE("lanmedia: video output and input over real UDP sockets, with a keyframe request", "[lanmedia][udp][video]") {
    VideoOutput::Options oo;
    oo.bindAddress = "127.0.0.1";
    oo.port = 0;
    VideoOutput out(oo);

    std::mutex m;
    std::vector<std::pair<std::uint64_t, bool>> got; // frame number, keyframe
    std::size_t bytes = 0;
    VideoInput::Options io;
    io.bindAddress = "127.0.0.1";
    VideoInput in(io, [&](const VideoReceiver::Frame& f) {
        std::lock_guard lock(m);
        got.emplace_back(f.number, f.keyframe);
        bytes += f.data.size();
    });
    CHECK_FALSE(out.hasDestinations());
    out.setDestinations({UdpTransport::Endpoint(asio::ip::make_address("127.0.0.1"), in.localPort())});
    CHECK(out.hasDestinations());

    // the viewer joins in the middle of a GOP: it asks for a keyframe, the encoder answers with one
    std::vector<std::uint8_t> au(20000);
    int sentKey = -1;
    for (int f = 0; f < 60; ++f) {
        for (std::size_t i = 0; i < au.size(); ++i) au[i] = static_cast<std::uint8_t>(f + i);
        const bool key = out.keyframeWanted();
        if (key && sentKey < 0) sentKey = f;
        out.send(au, key, nowSeconds());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(sentKey > 0);
    std::lock_guard lock(m);
    REQUIRE_FALSE(got.empty());
    CHECK(got.front().first == std::uint64_t(sentKey));
    CHECK(got.front().second);
    CHECK(got.size() == std::size_t(60 - sentKey));
    CHECK(bytes == got.size() * au.size());
    for (std::size_t i = 1; i < got.size(); ++i) CHECK(got[i].first == got[i - 1].first + 1);
    const auto s = out.stats();
    CHECK(s.keyframeRequests >= 1);
    CHECK(s.haveReport); // reports reach the output's socket
    CHECK(in.stats().lostDatagrams == 0);
}
