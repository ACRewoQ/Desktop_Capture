#pragma once

// Per-track recording (recorder): one take = one folder with one WAV / FLAC file per recorded strip.
// The audio thread pushes every recorded strip of every block into a lock-free FIFO per file (the
// mix plan holds the take), so all files start at the same block and stay the same length; a disk
// thread drains the FIFOs into the files. When the disk thread falls behind and a FIFO is full, the
// missing audio becomes silence in the file (the length is kept) and is counted as dropped.

#include "audio_file.hpp"
#include "plan.hpp"

#include "dc/clocksync/audio_fifo.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dc::mixd {

struct RecordStreamSpec {
    int strip = 0;    // 0 = master, otherwise the track number
    int channels = 2; // channels of the file
    Tap tap = Tap::Post;
    std::string fileName;
};

class RecorderTake {
public:
    static constexpr double kFifoSeconds = 4;

    // Creates the files in folder (which must exist) and starts the disk thread.
    static std::shared_ptr<RecorderTake> create(const std::filesystem::path& folder, AudioFileFormat format,
                                                std::vector<RecordStreamSpec> specs, std::string* error);
    ~RecorderTake();
    RecorderTake(const RecorderTake&) = delete;
    RecorderTake& operator=(const RecorderTake&) = delete;

    const std::filesystem::path& folder() const { return folder_; }
    const std::vector<RecordStreamSpec>& streams() const { return specs_; }

    // Audio thread. Every stream gets exactly `frames` frames per block: push() what was rendered,
    // pushSilence() for strips that are gone; then endBlock().
    void push(int stream, const float* interleaved, int frames);
    void pushSilence(int stream, int frames);
    void endBlock(int frames) { frames_.fetch_add(static_cast<std::uint64_t>(frames), std::memory_order_relaxed); }

    // Any thread.
    std::uint64_t frames() const { return frames_.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const; // frames filled with silence, the most of any file
    // Frames in the files; call only after finish() (the disk thread writes them until then).
    std::uint64_t fileFrames() const;
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    std::string error() const;

    // Control thread: writes what is still queued, closes the files and stops the disk thread.
    // Idempotent. False when a file could not be written completely (error() says why).
    bool finish();

private:
    struct Stream {
        Stream(int channels, std::size_t capacity) : fifo(channels, capacity) {}
        clocksync::AudioFifo fifo;
        std::atomic<std::uint64_t> lost{0};    // frames the audio thread could not queue
        std::atomic<std::uint64_t> dropped{0}; // total of lost, written as silence
        std::unique_ptr<AudioFileWriter> file;
        bool failed = false;
    };

    RecorderTake() = default;
    void run();
    bool drain(std::vector<float>& scratch);
    void fail(const std::string& message);

    std::filesystem::path folder_;
    std::vector<RecordStreamSpec> specs_;
    std::vector<std::unique_ptr<Stream>> streams_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> frames_{0};
    mutable std::mutex errorMutex_;
    std::string error_;
    bool finished_ = false;
    bool finishOk_ = true;
};

// "<Music>/Desktop Capture" (Windows) or ~/Music/Desktop Capture.
std::string defaultRecorderDirectory();

// File name part from a display name: characters Windows does not allow become "_".
std::string sanitizeFileName(const std::string& name);

} // namespace dc::mixd
