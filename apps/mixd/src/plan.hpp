#pragma once

// A MixPlan is everything the audio thread needs to mix one block, prepared off the audio thread from
// the parameter tree and the open ports. Plans are immutable once handed to the engine; a parameter
// change produces a new plan that the engine adopts at the next block boundary.

#include "ports.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace dc::mixd {

inline constexpr int kMaxStrips = 65;     // 0 = master, 1..64 = tracks
inline constexpr int kMaxBlockFrames = 1024; // largest chunk the mixer processes at once
inline constexpr int kMaxInputPorts = 32;
inline constexpr int kMaxOutputBuses = 32;
inline constexpr int kMaxInputChannels = 64;

struct Destination {
    enum class Kind : std::uint8_t { Track, Master, Bus };
    Kind kind = Kind::Master;
    int index = 0; // track number or bus index
};

struct SendPlan {
    Destination dest;
    float gain = 1;
    bool preFader = false;
    int slot = 0; // 0..7, keeps the send's smoothing state across plans
};

enum class Tap : std::uint8_t { None, Pre, Post };

struct StripPlan {
    int id = 0;       // 0 = master, otherwise the track number
    int channels = 2; // 1 = mono, 2 = stereo (master is always 2)
    int input = -1;   // index into MixPlan::inputs, -1 = no device input
    int inputChannel = 0; // first channel of the input port (0-based)
    bool inputDisconnected = false;
    float gain = 1;   // linear, already 0 when muted or silenced by another track's solo
    float pan = 0;
    bool lowcut = false;
    double lowcutHz = 80;
    bool highcut = false;
    double highcutHz = 18000;
    bool cue = false;
    std::vector<Destination> outputs;
    std::vector<SendPlan> sends;
    Tap shmTap = Tap::None; // export to shared memory slot id (pre: strip channels, post: stereo)
    std::uint16_t meterId = 0;
};

struct MixPlan {
    std::uint64_t generation = 0;
    int blockFrames = 256;
    std::vector<StripPlan> tracks; // processing order: every track comes before the tracks it feeds
    StripPlan master;
    std::vector<std::shared_ptr<InputPort>> inputs;
    std::vector<std::shared_ptr<OutputPort>> buses; // one stereo bus per output device in use
    int cueBus = -1;
    float cueGain = 1;
    // per input port: room for kMaxBlockFrames interleaved frames, filled by the engine each block
    mutable std::vector<std::vector<float>> inputScratch;
    float testToneGain = 0; // diagnostics: a sine added to the master input (mixd --test-tone)
    double testToneHz = 1000;
};

} // namespace dc::mixd
