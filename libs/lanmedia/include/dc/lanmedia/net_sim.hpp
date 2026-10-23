#pragma once

#include "dc/clocksync/tone_analyzer.hpp"
#include "dc/lanmedia/audio_receiver.hpp"
#include "dc/lanmedia/audio_sender.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dc::lan {

// Offline model of one LAN audio stream: a sender PC whose audio device records a sine tone, a network
// that delays, reorders, duplicates and loses datagrams, and a receiver PC whose audio device plays
// the AudioReceiver output. Both devices run on their own clocks. The simulation knows when every
// source frame was recorded, so it measures the end-to-end latency exactly, and a ToneAnalyzer
// listens to the output for clicks.
struct SimDevice {
    double ppm = 0;               // audio clock error
    std::size_t block = 480;      // frames per callback
    double callbackJitterSec = 0; // callback delay after the period boundary, uniform in [0, x]
};

struct SimNetwork {
    double baseDelayMs = 0.3;    // fastest one-way delay
    double jitterMeanMs = 0.05;  // extra delay per datagram, exponential with this mean
    double spikeProbability = 0; // chance that a datagram also gets a spike (Wi-Fi retry, power save)
    double spikeMs = 0;          // spike delay, uniform in [0, x]
    double lossProbability = 0;  // independent loss
    double burstLossEnter = 0;   // Gilbert model: chance to enter a loss burst per datagram
    double burstLossStay = 0;    // chance to stay in the burst
    double duplicateProbability = 0;
};

struct SimEvent {
    enum class Kind { Outage, SenderPause, SenderRestart } kind = Kind::Outage;
    double atSec = 0;
    double durationSec = 0; // Outage: all datagrams lost; SenderPause: the device delivers nothing
    double settleSec = 2;   // glitches and latency are not judged from atSec to atSec + duration + settle
};

struct SimExpect {
    bool glitchFree = true;         // no glitch block outside event windows
    double maxLatencyMs = 20;       // end-to-end, outside event windows
    double maxLatencySpreadMs = 3;  // max - min of end-to-end latency outside event windows
    std::uint64_t maxResyncs = 0;
    double maxConcealedPercent = 0; // concealed packets / packets sent
    double maxDriftErrorPpm = 20;   // mean resampling ratio against the true clock difference
};

struct SimConfig {
    std::string name;
    double seconds = 600;
    double warmupSeconds = 30; // excluded from the judgement (drift lock)
    double nominalRate = 48000;
    double toneHz = 997;
    double amplitude = 0.5;
    std::uint32_t seed = 1;
    SimDevice sender;
    SimDevice receiver;
    double receiverQpcPpm = 0; // the receiver PC's QPC against the sender PC's
    SimNetwork network;
    std::vector<SimEvent> events;
    AudioEncoder::Config encoder{};
    AudioReceiver::Config receiverConfig{};
    clocksync::ToneAnalyzer::Config analyzer{};
    SimExpect expect;
    double progressEverySec = 0; // 0 = no progress callback
};

struct SimProgress {
    double seconds = 0;
    AudioReceiver::Stats stats;
    double latencyMs = 0;
};

struct SimResult {
    AudioReceiver::Stats stats;
    AudioSender::Stats sender;
    clocksync::ToneAnalyzer::Summary tone; // after warm-up
    std::uint64_t glitchesOutsideEvents = 0;
    std::uint64_t datagramsSent = 0, datagramsLost = 0;
    double latencyMinMs = 0, latencyMaxMs = 0, latencyMeanMs = 0; // outside event windows, after warm-up
    double truePpm = 0;      // sender audio clock / receiver audio clock - 1
    double estimatedPpm = 0; // receiver ratio - 1, mean over the judged time
    SimExpect expect;
    std::vector<std::string> failures;
    bool passed() const { return failures.empty(); }
    std::string verdict() const;
};

SimResult runSimulation(const SimConfig& config, const std::function<void(const SimProgress&)>& progress = {});

// The scenarios run by dc-lan-sim (and, shortened, by the unit tests).
std::vector<SimConfig> standardScenarios();

} // namespace dc::lan
