#pragma once

// The mixing core: runs one MixPlan over a block of input audio. Deterministic and independent of
// devices and threads, so the tests drive it directly. Real-time safe: all buffers are allocated in
// the constructor.

#include "dsp.hpp"
#include "plan.hpp"
#include "sampler.hpp"

#include "dc/shmaudio/ring.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace dc::mixd {

// Peak and RMS per strip, accumulated by the audio thread and collected by the meter timer.
class MeterBank {
public:
    struct Reading {
        float peak[2] = {0, 0};
        float rms[2] = {0, 0};
        bool clipped = false;
        bool valid = false; // false when no audio was processed since the last read
    };

    // Audio thread.
    void add(int strip, const float* const* samples, int channels, int frames);
    // Any other thread: returns and clears what was accumulated since the last call.
    Reading take(int strip);

private:
    struct Accum {
        std::atomic<float> peak[2] = {0.0f, 0.0f};
        std::atomic<float> sumSq[2] = {0.0f, 0.0f};
        std::atomic<std::uint32_t> frames{0};
    };
    std::array<Accum, kMaxStrips> accum_{};
};

class Mixer {
public:
    Mixer();

    // Mixes frames (<= kMaxBlockFrames) with plan. inputs[i] holds frames interleaved samples of
    // plan.inputs[i] (its channel count), or nullptr for silence. Strips with a shared memory tap are
    // written to shm (when not null); the caller commits the block.
    void process(const MixPlan& plan, int frames, const float* const* inputs, shm::AudioRingWriter* shm);

    // Output of bus i of the last processed plan, one channel (0 = left, 1 = right).
    const float* bus(int index, int channel) const { return buses_[index][channel].data(); }

    MeterBank& meters() { return meters_; }
    Sampler& sampler() { return sampler_; }

private:
    using Buffer = std::array<float, kMaxBlockFrames>;
    struct StripState {
        bool present = false;
        Smoothed gain, panLeft, panRight;
        std::array<Smoothed, 8> sends;
        std::array<bool, 8> sendPresent{};
        StripFilter lowcut{Biquad::Type::HighPass};
        StripFilter highcut{Biquad::Type::LowPass};
    };

    void adopt(const MixPlan& plan);
    void initStrip(const StripPlan& s);
    void processStrip(const StripPlan& s, int frames, const float* const* inputs, const MixPlan& plan,
                      shm::AudioRingWriter* shm);
    float* destination(const Destination& d, int channel);
    void writeShm(shm::AudioRingWriter* shm, int slot, const float* const* x, int channels, int frames);
    void record(const MixPlan& plan, const StripPlan& s, const float* const* x, int channels, int frames);

    std::uint64_t generation_ = ~0ull;
    std::unique_ptr<std::array<StripState, kMaxStrips>> strips_;
    std::unique_ptr<std::array<std::array<Buffer, 2>, kMaxStrips>> acc_; // audio routed into each strip
    std::unique_ptr<std::array<std::array<Buffer, 2>, kMaxOutputBuses>> busesStorage_;
    std::array<std::array<Buffer, 2>, kMaxOutputBuses>& buses_;
    std::array<Buffer, 2> work_{}, post_{}, pre_{}, cue_{};
    std::array<float, kMaxBlockFrames * 2> interleave_{};
    std::array<bool, kMaxStrips> recorded_{}; // per take stream: written in this block
    double tonePhase_ = 0;
    MeterBank meters_;
    Sampler sampler_;
};

} // namespace dc::mixd
