#pragma once

// The engine's real-time part: adopts new mix plans at block boundaries, pulls the input ports, runs
// the mixer, feeds the output devices and the shared memory. render() is called by whatever clocks
// the engine (the clock output device, or the internal timer), always from one thread at a time.

#include "mixer.hpp"
#include "plan.hpp"

#include "dc/shmaudio/ring.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace dc::mixd {

class Engine {
public:
    explicit Engine(shm::AudioRingWriter* shm);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Control thread. The plan is adopted at the next block; plans that were replaced are deleted by
    // collectRetired(), never on the audio thread.
    void setPlan(std::unique_ptr<MixPlan> plan);
    void collectRetired();
    std::uint64_t adoptedGeneration() const { return adopted_.load(std::memory_order_acquire); }

    // Clock thread. Renders frames into out (interleaved, outChannels). clockPort is the output device
    // that out belongs to (nullptr for the internal timer); buses for other devices go to their
    // bridges. time is the QPC time of the block in seconds.
    void render(const OutputPort* clockPort, float* out, int frames, int outChannels, double time);

    Mixer& mixer() { return mixer_; }

    // Load of the last render calls: processing time / block duration, peak over the last ~0.5 s.
    double cpuLoad() const { return load_.load(std::memory_order_relaxed); }
    std::uint64_t framesRendered() const { return frames_.load(std::memory_order_relaxed); }

private:
    void adoptPending();

    shm::AudioRingWriter* shm_;
    Mixer mixer_;
    std::atomic<MixPlan*> pending_{nullptr};
    MixPlan* current_ = nullptr; // audio thread
    std::array<std::atomic<MixPlan*>, 64> retired_{}; // replaced plans, deleted by collectRetired()
    std::atomic<std::uint64_t> adopted_{0};
    std::array<float, kMaxBlockFrames * 2> interleave_{};
    std::array<const float*, kMaxInputPorts> inputPtrs_{};
    std::atomic<double> load_{0};
    double loadPeak_ = 0;
    std::uint64_t loadFrames_ = 0;
    std::atomic<std::uint64_t> frames_{0};
};

double qpcSecondsNow();

} // namespace dc::mixd
