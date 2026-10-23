// dc-lan-sim: offline check of the LAN media transport (audio streams).
//
//   dc-lan-sim [--minutes N] [--scenario NAME] [--list] [--quiet]
//
// Simulates one LAN audio stream for N minutes (default 10) per scenario: a sender PC recording a
// tone, a network with delay, jitter, reordering, duplicates, loss and outages, and a receiver PC on
// its own clock playing it through dc::lan::AudioReceiver. Measures the end-to-end latency exactly
// and listens for clicks. Exit code 0 when every scenario passes.

#include "dc/lanmedia/net_sim.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dc::lan;

namespace {

const char* codecName(AudioCodec c) {
    switch (c) {
    case AudioCodec::Pcm16: return "PCM16";
    case AudioCodec::Pcm24: return "PCM24";
    case AudioCodec::Opus: return "Opus";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv) {
    double minutes = 10;
    std::string only;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--minutes" && i + 1 < argc) minutes = std::atof(argv[++i]);
        else if (a == "--scenario" && i + 1 < argc) only = argv[++i];
        else if (a == "--quiet") quiet = true;
        else if (a == "--list") {
            for (const auto& s : standardScenarios()) std::printf("%s\n", s.name.c_str());
            return 0;
        } else {
            std::fprintf(stderr, "usage: dc-lan-sim [--minutes N] [--scenario NAME] [--list] [--quiet]\n");
            return 2;
        }
    }

    int failed = 0, ran = 0;
    for (auto s : standardScenarios()) {
        if (!only.empty() && s.name != only) continue;
        ++ran;
        s.seconds = minutes * 60;
        s.warmupSeconds = std::min(s.warmupSeconds, s.seconds / 4);
        // keep the events inside a shortened run
        for (auto& e : s.events) e.atSec = std::min(e.atSec, s.seconds / 2);
        s.progressEverySec = quiet ? 0 : 60;
        std::printf("== %s: %s %zu-frame packets, sender %+.0f ppm / %zu, receiver %+.0f ppm / %zu, "
                    "delay %.1f ms + jitter %.2f ms, loss %.2f %%, %.0f min\n",
                    s.name.c_str(), codecName(s.encoder.codec), s.encoder.frames, s.sender.ppm, s.sender.block,
                    s.receiver.ppm, s.receiver.block, s.network.baseDelayMs, s.network.jitterMeanMs,
                    100 * s.network.lossProbability, minutes);
        std::fflush(stdout);
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = runSimulation(s, [](const SimProgress& p) {
            std::printf("  %6.0f s  ratio %+8.2f ppm  fill %6.2f ms  target %6.2f ms  jitter %6.2f ms  latency %6.2f ms\n",
                        p.seconds, (p.stats.ratio - 1) * 1e6, p.stats.fillMs, p.stats.targetMs,
                        p.stats.latenessPeakMs, p.latencyMs);
            std::fflush(stdout);
        });
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  %s  (%.1f s)\n", r.verdict().c_str(), wall);
        if (r.sender.haveReport)
            std::printf("  sender sees: received %u, lost %u, late %u, concealed %u, jitter %.2f ms, buffer %.2f ms\n",
                        r.sender.report.received, r.sender.report.lost, r.sender.report.late,
                        r.sender.report.concealed, r.sender.report.jitterUs / 1000.0, r.sender.report.latencyUs / 1000.0);
        for (const auto& e : r.tone.events)
            std::printf("    glitch block %llu: SINAD %.1f dB, amplitude %.3f\n", (unsigned long long)e.index,
                        e.sinadDb, e.amplitude);
        std::fflush(stdout);
        if (!r.passed()) ++failed;
    }
    if (ran == 0) {
        std::fprintf(stderr, "unknown scenario: %s\n", only.c_str());
        return 2;
    }
    std::printf("\n%s (%d of %d scenarios passed)\n", failed ? "SIMULATION FAILED" : "ALL SCENARIOS PASSED",
                ran - failed, ran);
    return failed ? 1 : 0;
}
