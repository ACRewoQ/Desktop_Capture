// Reads the recording audio from the engine's shared memory (docs/spec/shm-audio.md) and puts it on the
// recording's time line: output sample 0 is the recording start (a QPC time shared with the video), and the
// output never stops or slows down. While the engine is not running (or stalls) the gap is filled with silence,
// and when it comes back the reader is re-aligned by the time anchors, so audio stays in sync with the video.
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
        std::uint64_t resyncs = 0;  // re-alignments (engine restart, drift)
        bool engineConnected = false;
        double driftMs = 0;  // engine time of the newest sample minus the recording's time for it
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

private:
    void run();
    bool ensureAttached(std::int64_t nowQpc);
    void sync(std::int64_t nowQpc);
    void emitSilence(std::uint64_t frames);
    void emit(std::uint32_t frames);
    std::int64_t framesAt(std::int64_t qpc) const;
    void checkDrift(std::int64_t nowQpc);

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
    std::int64_t offset_ = 0;  // engine frame = offset_ + output frame
    std::int64_t nextDriftCheckQpc_ = 0;

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
