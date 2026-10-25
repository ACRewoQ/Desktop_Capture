#pragma once

#include "dc/lanmedia/net_sim.hpp"
#include "dc/lanmedia/video.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace dc::lan {

// Offline model of one LAN video stream: an encoder that produces access units at a fixed frame rate
// (periodic keyframes, plus one whenever the sender says a receiver asked), a network with a link
// rate (so a keyframe leaves as a burst that takes time to serialize), delay, jitter, loss and
// duplication, and a receiver that is polled every few milliseconds and sends its reports back over
// the same network. It checks that every delivered frame can be decoded (a non-keyframe only follows
// the frame before it) and measures latency, freezes and how quickly a lost frame is repaired.
struct VideoSimEncoder {
    int fps = 30;
    int gopFrames = 300;           // periodic keyframe interval (0 = only on request)
    std::size_t keyframeBytes = 40000;
    std::size_t frameBytes = 12000; // other frames, +-30 % random
    double encodeMs = 3;            // capture -> access unit
};

struct VideoSimEvent {
    enum class Kind { Outage, SenderRestart, Join } kind = VideoSimEvent::Kind::Outage;
    double atSec = 0;
    double durationSec = 0; // Outage
};

struct VideoSimExpect {
    double minDeliveredPercent = 99.9; // delivered frames / frames sent while the receiver listens
    double maxLatencyMs = 10;          // capture -> complete at the receiver, 99th percentile
    double maxFreezeMs = 100;          // longest gap between delivered frames (capture time), outside events
    double maxFirstFrameMs = 150;      // after the start or a Join: first picture
};

struct VideoSimConfig {
    std::string name;
    double seconds = 120;
    std::uint32_t seed = 1;
    VideoSimEncoder encoder;
    SimNetwork network;
    double linkMbps = 1000;  // bottleneck link rate
    double pollMs = 2;
    std::vector<VideoSimEvent> events;
    VideoReceiver::Config receiver{};
    VideoSimExpect expect;
};

struct VideoSimResult {
    VideoReceiver::Stats stats;
    VideoSender::Stats sender;
    std::uint64_t framesSent = 0, framesDelivered = 0, undecodable = 0;
    std::uint64_t datagramsSent = 0, datagramsLost = 0;
    double latencyMeanMs = 0, latencyP99Ms = 0, latencyMaxMs = 0;
    double freezeMaxMs = 0;      // outside event windows
    double firstFrameMaxMs = 0;  // worst time to first picture (start, joins, restarts)
    double deliveredPercent = 0;
    VideoSimExpect expect;
    std::vector<std::string> failures;
    bool passed() const { return failures.empty(); }
    std::string verdict() const;
};

VideoSimResult runVideoSimulation(const VideoSimConfig& config);

// Scenarios run by dc-lan-sim --video (and, shortened, by the unit tests).
std::vector<VideoSimConfig> standardVideoScenarios();

} // namespace dc::lan
