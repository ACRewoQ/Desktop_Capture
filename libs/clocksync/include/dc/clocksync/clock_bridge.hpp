#pragma once

#include "dc/clocksync/audio_fifo.hpp"
#include "dc/clocksync/drift_controller.hpp"
#include "dc/clocksync/resampler.hpp"
#include "dc/clocksync/time_dll.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dc::clocksync {

// Carries audio from a device running on one clock (producer, e.g. a capture device) to a device or
// engine running on another clock (consumer, e.g. the engine's master output). Both run at the same
// nominal rate; the bridge measures their real rate difference and corrects it with a variable-rate
// resampler, so the latency stays constant and the FIFO never runs dry or overflows.
//
// How the drift is measured: the producer timestamps its packets; a DLL turns them into "producer
// frame position at time t". At each consumer block the bridge compares that position with the
// position it is reading, which gives a fill level free of the sawtooth that bursty packets cause.
// A slow PI loop keeps that fill at the value it had when playback started.
//
// Threads: push() and producerDiscontinuity() on the producer thread, pull() on the consumer thread.
// Both are real-time safe. stats() may be called from any thread.
class ClockBridge {
public:
    struct Config {
        int channels = 2;
        double nominalRate = 48000;
        std::size_t targetFrames = 1440;  // FIFO level to hold (30 ms at 48 kHz)
        std::size_t fifoFrames = 16384;   // must exceed target + both device periods
        std::size_t maxBlockFrames = 4096; // largest pull() block
        double dllBandwidthHz = 0.5;
        DriftController::Config controller{};
        VariableResampler::Quality quality{};
    };

    enum class State : int { Waiting = 0, Running = 1 };

    struct Stats {
        State state = State::Waiting;
        double ratio = 1;          // input frames per output frame
        double fillError = 0;      // filtered fill - setpoint, frames
        double fifoFrames = 0;     // FIFO level at the last pull
        double producerRate = 0;   // producer clock estimate, frames per second
        std::uint64_t underruns = 0; // consumer found too little data: silence was output, then resync
        std::uint64_t overruns = 0;  // producer found the FIFO full: data was dropped, then resync
        std::uint64_t resyncs = 0;   // restarts after a discontinuity reported by the producer
        std::uint64_t outputFrames = 0;
        bool locked = false;
    };

    ClockBridge() : ClockBridge(Config{}) {}
    explicit ClockBridge(Config config);

    // Producer: frames (interleaved), and when the first of them was sampled, in seconds on the
    // clock shared with the consumer (QPC).
    void push(const float* frames, std::size_t count, double timeOfFirstFrame);
    // Producer: the device reported a gap (lost data). The consumer restarts the lock.
    void producerDiscontinuity();

    // Consumer: fills count frames. now = time of this block on the shared clock (the time the
    // consumer device takes its first frame, or simply the callback time).
    void pull(float* out, std::size_t count, double now);

    Stats stats() const;
    const Config& config() const { return cfg_; }

private:
    struct Stamp {
        std::uint64_t pos;
        double time;
    };

    void restart(bool keepRatio);
    void publish();

    Config cfg_;
    AudioFifo fifo_;
    SpscQueue<Stamp, 256> stamps_;
    std::atomic<bool> producerGap_{false};
    std::atomic<std::uint64_t> overruns_{0};

    // consumer-owned
    TimeDll dll_;
    DriftController controller_;
    VariableResampler resampler_;
    State state_ = State::Waiting;
    bool haveSetpoint_ = false;
    double setpoint_ = 0;
    double lastPull_ = 0;
    std::uint64_t underruns_ = 0;
    std::uint64_t resyncs_ = 0;
    std::uint64_t outputFrames_ = 0;

    // published for stats()
    std::atomic<int> sState_{0};
    std::atomic<double> sRatio_{1}, sFillError_{0}, sFifo_{0}, sRate_{0};
    std::atomic<std::uint64_t> sUnderruns_{0}, sResyncs_{0}, sOutput_{0};
    std::atomic<bool> sLocked_{false};
};

} // namespace dc::clocksync
