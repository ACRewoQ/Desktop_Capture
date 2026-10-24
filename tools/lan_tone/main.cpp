// dc-lan-tone: sends a test tone over the LAN media transport and checks it on the other side, over
// real UDP sockets. No audio devices: the sender and the receiver are paced by the PC clock, so it
// measures what the network and the transport add (loss, jitter, buffer level, round trip).
//
//   dc-lan-tone recv [--port P] [--seconds S] [--block FRAMES]
//   dc-lan-tone send --to HOST[:PORT] [--codec pcm24|pcm16|opus] [--packet FRAMES] [--block FRAMES] [--seconds S]
//   dc-lan-tone loop [--codec ...] [--packet FRAMES] [--block FRAMES] [--seconds S]   (both, over 127.0.0.1)
//
// The receiver prints its statistics every second and a verdict at the end: exit code 0 when nothing
// was lost, late or concealed after the first 5 seconds and the tone had no clicks.

#include "dc/clocksync/tone_analyzer.hpp"
#include "dc/lanmedia/audio_receiver.hpp"
#include "dc/lanmedia/audio_sender.hpp"
#include "dc/lanmedia/udp_transport.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

using namespace dc::lan;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 997;
constexpr double kWarmup = 5;

struct Options {
    std::string mode;
    std::string to;
    std::uint16_t port = kDefaultMediaPort;
    AudioCodec codec = AudioCodec::Pcm24;
    std::size_t packet = 0; // 0 = codec default
    std::size_t block = 480;
    double seconds = 60;
};

void usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  dc-lan-tone recv [--port P] [--seconds S] [--block FRAMES]\n"
                 "  dc-lan-tone send --to HOST[:PORT] [--codec pcm24|pcm16|opus] [--packet FRAMES] [--block FRAMES] "
                 "[--seconds S]\n"
                 "  dc-lan-tone loop [--codec ...] [--packet FRAMES] [--block FRAMES] [--seconds S]\n");
}

std::optional<Options> parse(int argc, char** argv) {
    if (argc < 2) return std::nullopt;
    Options o;
    o.mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = (a.rfind("--", 0) == 0) ? next() : nullptr;
        if (!v) return std::nullopt;
        if (a == "--to") o.to = v;
        else if (a == "--port") o.port = static_cast<std::uint16_t>(std::atoi(v));
        else if (a == "--packet") o.packet = std::size_t(std::atoi(v));
        else if (a == "--block") o.block = std::size_t(std::atoi(v));
        else if (a == "--seconds") o.seconds = std::atof(v);
        else if (a == "--codec") {
            const std::string c = v;
            if (c == "pcm24") o.codec = AudioCodec::Pcm24;
            else if (c == "pcm16") o.codec = AudioCodec::Pcm16;
            else if (c == "opus") o.codec = AudioCodec::Opus;
            else return std::nullopt;
        } else return std::nullopt;
    }
    if (o.mode != "recv" && o.mode != "send" && o.mode != "loop") return std::nullopt;
    if (o.mode == "send" && o.to.empty()) return std::nullopt;
    if (o.packet == 0) o.packet = o.codec == AudioCodec::Opus ? 480 : 120;
    if (o.block == 0 || o.block > 4096) return std::nullopt;
    return o;
}

// Calls fn(k, t) for k = 0, 1, ... at t = start + k * period on the PC clock until `seconds` passed.
template <typename F> void paced(double period, double seconds, const std::atomic<bool>& stop, F&& fn) {
    const double start = nowSeconds();
    for (std::uint64_t k = 0;; ++k) {
        const double t = start + double(k) * period;
        if (t - start >= seconds || stop.load()) break;
        double now = nowSeconds();
        while (now < t) {
            const double wait = t - now;
            if (wait > 0.002) std::this_thread::sleep_for(std::chrono::duration<double>(wait - 0.0015));
            else std::this_thread::yield();
            now = nowSeconds();
        }
        fn(k, t);
    }
}

int runSender(const Options& o, UdpTransport& net, const UdpTransport::Endpoint& to, const std::atomic<bool>& stop,
              bool quiet) {
    AudioSender::Config c;
    c.encoder.codec = o.codec;
    c.encoder.frames = o.packet;
    AudioSender sender(c);
    std::vector<float> block(o.block * 2);
    double nextPrint = 1;
    std::mutex reportMutex; // onReport comes from the receive thread, pump from this one
    net.start([&](std::span<const std::uint8_t> d, const UdpTransport::Endpoint&, double t) {
        if (const auto r = parseReport(d)) {
            std::lock_guard lock(reportMutex);
            sender.onReport(*r, t);
        }
    });
    std::printf("sending stream %08x to %s:%u, %zu-frame packets, %zu-frame blocks\n", sender.streamId(),
                to.address().to_string().c_str(), to.port(), o.packet, o.block);
    paced(double(o.block) / 48000.0, o.seconds, stop, [&](std::uint64_t k, double t) {
        for (std::size_t i = 0; i < o.block; ++i) {
            const double ph = 2 * kPi * kToneHz * double(k * o.block + i) / 48000.0;
            block[2 * i] = float(0.5 * std::sin(ph));
            block[2 * i + 1] = float(0.5 * std::sin(ph + 0.5));
        }
        sender.push(block.data(), o.block, t - double(o.block) / 48000.0);
        {
            std::lock_guard lock(reportMutex);
            sender.pump([&](std::span<const std::uint8_t> d) { net.send(d, to); });
        }
        const double elapsed = double((k + 1) * o.block) / 48000.0;
        if (!quiet && elapsed >= nextPrint) {
            nextPrint += 1;
            const auto s = sender.stats();
            if (s.haveReport)
                std::printf("[send %5.0f s] packets %llu, receiver: lost %u, late %u, concealed %u, jitter %.2f ms, "
                            "buffer %.2f ms, rtt %.2f ms\n",
                            elapsed, (unsigned long long)s.packets, s.report.lost, s.report.late, s.report.concealed,
                            s.report.jitterUs / 1000.0, s.report.latencyUs / 1000.0, s.rttMs);
            else
                std::printf("[send %5.0f s] packets %llu (no report from the receiver yet)\n", elapsed,
                            (unsigned long long)s.packets);
            std::fflush(stdout);
        }
    });
    net.stop();
    return 0;
}

int runReceiver(const Options& o, UdpTransport& net, const std::atomic<bool>& stop) {
    AudioReceiver receiver({});
    std::mutex peerMutex;
    std::optional<UdpTransport::Endpoint> peer;
    net.start([&](std::span<const std::uint8_t> d, const UdpTransport::Endpoint& from, double t) {
        if (receiver.onPacket(d, t)) {
            std::lock_guard lock(peerMutex);
            peer = from;
        }
    });
    std::printf("receiving on port %u, %zu-frame blocks\n", net.localPort(), o.block);
    std::fflush(stdout);

    dc::clocksync::ToneAnalyzer::Config ac;
    ac.toneHz = kToneHz;
    std::vector<float> block(o.block * 2);
    std::optional<dc::clocksync::ToneAnalyzer> analyzer;
    AudioReceiver::Stats atWarmup{};
    double runningSince = -1, nextPrint = 1;
    paced(double(o.block) / 48000.0, o.seconds, stop, [&](std::uint64_t k, double t) {
        receiver.pull(block.data(), o.block, t);
        const double elapsed = double((k + 1) * o.block) / 48000.0;
        const auto s = receiver.stats();
        if (s.state == AudioReceiver::State::Running && runningSince < 0) runningSince = elapsed;
        if (runningSince >= 0 && !analyzer && elapsed - runningSince >= kWarmup) {
            ac.glitchSinadDb = s.codec == AudioCodec::Opus ? 20 : 60;
            analyzer.emplace(ac);
            atWarmup = s;
        }
        if (analyzer) analyzer->feed(block.data(), o.block, 2);
        if (elapsed >= nextPrint) {
            nextPrint += 1;
            std::optional<UdpTransport::Endpoint> to;
            {
                std::lock_guard lock(peerMutex);
                to = peer;
            }
            if (to) net.send(encodeReport(receiver.makeReport(nowSeconds())), *to);
            std::printf("[recv %5.0f s] stream %08x, received %llu, lost %llu, late %llu, concealed %llu, FEC %llu, "
                        "fill %.2f ms, target %.2f ms, jitter %.2f ms, drift %+.1f ppm\n",
                        elapsed, s.streamId, (unsigned long long)s.received, (unsigned long long)s.lost,
                        (unsigned long long)s.late, (unsigned long long)s.concealed,
                        (unsigned long long)s.fecRecovered, s.fillMs, s.targetMs, s.latenessPeakMs,
                        (s.ratio - 1) * 1e6);
            std::fflush(stdout);
        }
    });
    net.stop();

    const auto s = receiver.stats();
    if (!analyzer) {
        std::printf("FAIL: the stream did not play for %.0f s\n", kWarmup);
        return 1;
    }
    const auto& tone = analyzer->summary();
    const auto lost = s.lost - atWarmup.lost, late = s.late - atWarmup.late,
               concealed = s.concealed - atWarmup.concealed, resyncs = s.resyncs - atWarmup.resyncs;
    const bool ok = lost == 0 && late == 0 && concealed == 0 && resyncs == 0 && tone.glitchBlocks == 0;
    std::printf("%s: after warm-up lost %llu, late %llu, concealed %llu, resyncs %llu, glitch blocks %llu/%llu "
                "(min SINAD %.1f dB), buffer target %.2f ms, jitter peak %.2f ms\n",
                ok ? "PASS" : "FAIL", (unsigned long long)lost, (unsigned long long)late,
                (unsigned long long)concealed, (unsigned long long)resyncs, (unsigned long long)tone.glitchBlocks,
                (unsigned long long)tone.blocks, tone.minSinadDb, s.targetMs, s.latenessPeakMs);
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    const auto opt = parse(argc, argv);
    if (!opt) {
        usage();
        return 2;
    }
#ifdef _WIN32
    timeBeginPeriod(1); // 1 ms sleep resolution for the pacing loops
#endif
    const Options& o = *opt;
    std::atomic<bool> stop{false};
    try {
        if (o.mode == "recv") {
            UdpTransport net({"0.0.0.0", o.port});
            return runReceiver(o, net, stop);
        }
        if (o.mode == "send") {
            std::string host = o.to;
            std::uint16_t port = kDefaultMediaPort;
            if (const auto colon = host.rfind(':'); colon != std::string::npos) {
                port = static_cast<std::uint16_t>(std::atoi(host.c_str() + colon + 1));
                host.resize(colon);
            }
            UdpTransport net({"0.0.0.0", 0});
            return runSender(o, net, UdpTransport::resolve(host, port), stop, false);
        }
        // loop: both ends in this process over the loopback interface
        UdpTransport rxNet({"127.0.0.1", 0});
        UdpTransport txNet({"127.0.0.1", 0});
        const auto to = UdpTransport::Endpoint(asio::ip::make_address("127.0.0.1"), rxNet.localPort());
        int rc = 0;
        std::thread rx([&] { rc = runReceiver(o, rxNet, stop); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        runSender(o, txNet, to, stop, true);
        rx.join();
        return rc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
