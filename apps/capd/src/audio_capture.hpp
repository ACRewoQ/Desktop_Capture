// Reads the recording audio from the engine's shared memory (docs/spec/shm-audio.md) and puts it on the
// recording's time line: output sample 0 is the recording start (a QPC time shared with the video), and the
// output never stops or slows down. While the engine is not running (or stalls) the gap is filled with silence,
// and when it comes back the reader is re-aligned by the time anchors, so audio stays in sync with the video.
//
// The engine's sample clock (its output device) and QPC drift apart by up to a few hundred ppm. The reader
// measures that drift from the time anchors and follows it with a variable-rate resampler (libs/clocksync, the
// same PI loop and sinc interpolator as the engine's ClockBridge), so the recording stays aligned without the
// audible skip or repeat a re-alignment makes. A hard re-alignment remains only for jumps (engine restart, stall).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "dc/clocksync/drift_controller.hpp"
#include "dc/clocksync/resampler.hpp"
#include "dc/shmaudio/ring.hpp"
#include "dc/shmaudio/shared_memory.hpp"

namespace dc::capd {

class AudioCapture {
public:
    // One block for every stream, all of the same length: interleaved stereo float, frames * 2 samples each.
    using Sink = std::function<void(const std::vector<std::span<const float>>& streams, std::uint32_t frames)>;

    struct Stats {
        std::uint64_t frames = 0;         // samples per stream delivered so far (the recording's audio clock)
        std::uint64_t silenceFrames = 0;  // of those, filled with silence (engine absent, stalled or overrun)
        std::uint64_t overruns = 0;
        std::uint64_t resyncs = 0;  // hard re-alignments (engine start or restart, stall, jump over kMaxDriftMs)
        bool engineConnected = false;
        double driftMs = 0;  // engine time of the newest sample minus the recording's time for it
        double ratio = 1;    // engine frames consumed per recorded frame (1 + the measured clock difference)
    };

    // slots: shared-memory slot per output stream (0 = master, n = track n). shmSuffix: test-only mapping name.
    AudioCapture(std::vector<std::uint32_t> slots, std::string shmSuffix = "");
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // Starts the reading thread. startQpc is the recording's time zero.
    void start(std::int64_t startQpc, Sink sink);

    // Delivers audio up to endQpc (waiting briefly for the engine to commit it, silence otherwise), then stops.
    void stop(std::int64_t endQpc);

    Stats stats() const;

    // Without a thread (tests): set up, then call pump() with explicit times.
    void begin(std::int64_t startQpc, Sink sink);
    // Delivers audio up to (now - lagFrames). Returns the number of frames delivered.
    std::uint64_t pump(std::int64_t nowQpc, std::uint32_t lagFrames);

    // How far behind real time the reader stays, so the engine has committed the block (100 ms).
    static constexpr std::uint32_t kLagFrames = 4800;
    // Alignment error that is corrected by a jump instead of the resampler (engine glitch rather than drift).
    static constexpr double kMaxDriftMs = 40.0;

private:
    void run();
    bool ensureAttached(std::int64_t nowQpc);
    std::int64_t sync(std::int64_t nowQpc);
    void emitSilence(std::uint64_t frames);
    void emit(std::uint32_t frames);
    std::int64_t framesAt(std::int64_t qpc) const;
    // Alignment error in frames at the next output frame: positive = reading older audio than due (consume faster).
    std::optional<double> alignmentError() const;

    std::vector<std::uint32_t> slots_;
    std::string shmSuffix_;
    Sink sink_;
    std::int64_t startQpc_ = 0;
    std::int64_t freq_ = 1;

    shm::SharedMemory memory_;
    std::optional<shm::AudioRingReader> reader_;
    std::int64_t nextAttachQpc_ = 0;
    std::int64_t lastAliveQpc_ = 0;
    bool needsSync_ = true;
    bool needsPrime_ = false;  // after a re-alignment: give the resamplers real history (see pump)
    std::uint64_t readFrame_ = 0;  // next engine frame to hand to the resamplers

    clocksync::DriftController controller_;
    std::vector<clocksync::VariableResampler> resamplers_;  // one per stream, all fed and run alike
    std::vector<std::vector<float>> buffers_;
    std::vector<std::span<const float>> views_;

    mutable std::mutex statsMutex_;
    Stats stats_;
    std::uint64_t position_ = 0;  // output frames delivered

    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::int64_t> endQpc_{0};
};

}  // namespace dc::capd
