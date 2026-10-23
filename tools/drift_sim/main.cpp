// dc-drift-sim: offline check of the clock drift correction (verification V2).
//
//   dc-drift-sim [--minutes N] [--scenario NAME] [--list] [--quiet]
//
// Simulates two audio devices on independent clocks connected by dc::clocksync::ClockBridge for N
// minutes (default 60) per scenario, listens to the output for clicks and measures the drift of the
// end-to-end latency. Exit code 0 when every scenario passes.

#include "dc/clocksync/drift_sim.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace dc::clocksync;

namespace {

std::vector<SimConfig> scenarios() {
    std::vector<SimConfig> list;
    auto add = [&](const char* name, SimClock p, SimClock c) {
        SimConfig s;
        s.name = name;
        s.producer = p;
        s.consumer = c;
        s.bridge.channels = 1; // the bridge is channel-agnostic; mono keeps the hour-long runs fast
        // the FIFO level swings by one producer packet; hold enough for both periods plus 10 ms
        s.bridge.targetFrames = std::max<std::size_t>(1440, p.period + c.period + 480);
        list.push_back(s);
    };
    // Typical: two WASAPI devices with 10 ms periods, small crystal errors, some scheduling jitter.
    add("typical", {+40, 0, 600, 480, 0.002, 20e-6}, {-25, 0, 600, 480, 0.002, 20e-6});
    // Cheap USB device far off nominal, small consumer period.
    add("large-offset", {+500, 0, 600, 480, 0.002, 20e-6}, {-500, 0, 600, 144, 0.001, 20e-6});
    // Temperature wander: the difference moves by +-20 ppm with a 10 minute period.
    add("wander", {+30, 20, 600, 480, 0.002, 20e-6}, {0, 0, 600, 480, 0.002, 20e-6});
    // Unequal packet sizes and noisy timestamps (USB / Bluetooth style bursts).
    add("bursty", {-60, 5, 900, 1024, 0.004, 1e-3}, {+15, 0, 600, 441, 0.002, 200e-6});
    // Same crystal (both devices on one clock): nothing to correct, must stay still.
    add("same-clock", {+10, 0, 600, 480, 0.002, 20e-6}, {+10, 0, 600, 480, 0.002, 20e-6});
    return list;
}

} // namespace

int main(int argc, char** argv) {
    double minutes = 60;
    std::string only;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--minutes" && i + 1 < argc) minutes = std::atof(argv[++i]);
        else if (a == "--scenario" && i + 1 < argc) only = argv[++i];
        else if (a == "--quiet") quiet = true;
        else if (a == "--list") {
            for (const auto& s : scenarios()) std::printf("%s\n", s.name.c_str());
            return 0;
        } else {
            std::fprintf(stderr, "usage: dc-drift-sim [--minutes N] [--scenario NAME] [--list] [--quiet]\n");
            return 2;
        }
    }

    int failed = 0, ran = 0;
    for (auto s : scenarios()) {
        if (!only.empty() && s.name != only) continue;
        ++ran;
        s.seconds = minutes * 60;
        s.warmupSeconds = std::min(60.0, s.seconds / 4);
        s.progressEverySec = quiet ? 0 : 300;
        std::printf("== %s: producer %+.0f ppm (+-%.0f), consumer %+.0f ppm, periods %zu/%zu, %.0f min\n",
                    s.name.c_str(), s.producer.ppm, s.producer.wanderPpm, s.consumer.ppm, s.producer.period,
                    s.consumer.period, minutes);
        std::fflush(stdout);
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = runSimulation(s, [](const SimProgress& p) {
            std::printf("  %6.0f s  true %+8.2f ppm  est %+8.2f ppm  fill err %+7.2f  fifo %5.0f  latency %.3f ms\n",
                        p.seconds, p.truePpm, (p.stats.ratio - 1) * 1e6, p.stats.fillError, p.stats.fifoFrames,
                        p.latencyMs);
            std::fflush(stdout);
        });
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  fill error %+.2f..%+.2f frames, FIFO %.0f..%.0f frames, max ratio step %.4f ppm\n",
                    r.fillErrorMin, r.fillErrorMax, r.fifoMin, r.fifoMax, r.maxRatioStepPpm);
        std::printf("  %s  (%.1f s)\n", r.verdict().c_str(), wall);
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
