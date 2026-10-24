#pragma once

// Ports connect device streams to the engine. A port outlives the device stream behind it for as long
// as a mix plan refers to it (shared_ptr), so the device manager can close a stream at any time: the
// engine then simply reads silence until the next plan drops the port.

#include "dc/clocksync/clock_bridge.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace dc::lan {
class AudioReceiver;
class AudioSender;
} // namespace dc::lan

namespace dc::mixd {

// Audio captured by an input device, carried to the engine clock by a ClockBridge.
struct InputPort {
    InputPort(std::string deviceId, int channelCount, const clocksync::ClockBridge::Config& cfg)
        : id(std::move(deviceId)), channels(channelCount), bridge(cfg) {}

    const std::string id;
    const int channels; // interleaved channels of the bridge (the device's channel count)
    clocksync::ClockBridge bridge;
    std::atomic<bool> streaming{false}; // a device stream is currently pushing into the bridge
    // LAN input (key "lan:<peer>/<n>"): the engine pulls this receiver instead of the bridge. streaming
    // then means "the stream is playing".
    std::shared_ptr<lan::AudioReceiver> lan;
};

// Audio the engine sends to an output device. The device that clocks the engine is written directly
// from the engine's render call; any other output device gets its audio through the bridge.
struct OutputPort {
    OutputPort(std::string deviceId, const clocksync::ClockBridge::Config& cfg) : id(std::move(deviceId)), bridge(cfg) {}

    const std::string id;
    clocksync::ClockBridge bridge;      // stereo, engine -> device
    std::atomic<bool> streaming{false}; // a device stream pulls from the bridge (not the clock device)
    // LAN send stream (key "lan:<n>"): the engine pushes the bus into this sender instead of the bridge.
    std::shared_ptr<lan::AudioSender> lan;
};

} // namespace dc::mixd
