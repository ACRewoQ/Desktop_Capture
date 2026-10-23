#pragma once

#include "dc/clocksync/clock_bridge.hpp"
#include "dc/clocksync/tone_analyzer.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace dc::clocksync {

// Offline model of two audio devices on independent clocks, connected by a ClockBridge.
// The producer device records a sine tone, the consumer device plays the bridge output, and a
// ToneAnalyzer listens to what the consumer plays. Because the simulation knows the true time of
// every frame, it measures the end-to-end latency exactly and reports how much it drifted.
struct SimClock {
    double ppm = 0;               // constant clock error
    double wanderPpm = 0;         // additional sinusoidal error (temperature drift)
    double wanderPeriodSec = 600;
    std::size_t period = 480;     // frames per packet / callback
    double callbackJitterSec = 0; // callback delay after the period boundary, uniform in [0, x]
    double timestampJitterSec = 0; // error of the reported timestamp, Gaussian sigma
};

struct SimConfig {
    std::string name;
    SimClock producer;
    SimClock consumer;
    double seconds = 3600;
    double warmupSeconds = 60; // excluded from latency and fill statistics (lock phase)
    double nominalRate = 48000;
    double toneHz = 1000;
    double amplitude = 0.5;
    std::uint32_t seed = 1;
    ClockBridge::Config bridge{};
    ToneAnalyzer::Config analyzer{};
    double progressEverySec = 0; // 0 = no progress callback
};

struct SimProgress {
    double seconds = 0;
    ClockBridge::Stats stats;
    double truePpm = 0;   // producer rate / consumer rate - 1, in ppm, at this moment
    double latencyMs = 0; // end-to-end latency (relative: includes an arbitrary whole number of tone periods)
};

struct SimResult {
    ClockBridge::Stats stats;
    ToneAnalyzer::Summary tone; // after warm-up
    double truePpm = 0;         // at the end
    double estimatedPpm = 0;    // bridge ratio at the end
    double latencyDriftMs = 0;  // max - min of end-to-end latency after warm-up
    double fillErrorMin = 0, fillErrorMax = 0; // frames, after warm-up
    double fifoMin = 0, fifoMax = 0;           // frames, after warm-up
    double maxRatioStepPpm = 0; // largest ratio change between two consumer blocks after warm-up
    bool passed() const;
    std::string verdict() const;
};

SimResult runSimulation(const SimConfig& config, const std::function<void(const SimProgress&)>& progress = {});

} // namespace dc::clocksync
