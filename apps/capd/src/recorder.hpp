// One recording: video from a mix (Windows) and the audio streams from the engine's shared memory, encoded and
// written into one MKV/MP4 file. The recording's clock is QPC: time zero is the moment start() runs, video frames
// get their pts from the mix tick time and audio from the sample count since time zero, so both stay aligned.
#pragma once

#include "settings.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dc::capd {

class AudioCapture;
class AudioEncoder;
class Muxer;
#ifdef _WIN32
class Converter;
class VideoEncoder;
class VideoMix;
#endif

class Recorder {
public:
    struct Status {
        std::string file;
        std::int64_t elapsedMs = 0;
        std::uint64_t videoFrames = 0;   // frames handed to the encoder
        std::uint64_t videoDropped = 0;  // encoder pool exhausted (the encoder fell behind)
        std::uint64_t bytes = 0;
        std::uint64_t audioFrames = 0;
        std::uint64_t audioSilenceFrames = 0;
        bool engineConnected = false;
        double audioDriftMs = 0;
        std::uint64_t diskFreeBytes = 0;  // on the recording's drive, checked once a second (0 = unknown)
        std::string error;  // non-empty: the recording failed and must be stopped
    };

    // Free bytes on the drive holding a path; nullopt when it cannot be read. Replaceable for tests.
    using FreeSpaceProbe = std::function<std::optional<std::uint64_t>(const std::string& path)>;
    void setFreeSpaceProbe(FreeSpaceProbe probe) { freeSpace_ = std::move(probe); }

#ifdef _WIN32
    // mix may be null for an audio-only recording.
    Recorder(const RecordSettings& s, VideoMix* mix, std::string path, std::string shmSuffix = "");
#else
    Recorder(const RecordSettings& s, std::string path, std::string shmSuffix = "");
#endif
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Opens encoders and the file and starts capturing. Throws with a readable message on failure (also when the
    // drive has less than twice diskReserveBytes() free).
    void start();
    // Stops and finalizes the file. Returns the final status (error set if finalizing failed).
    Status stop();

    // Also checks the free disk space (once a second): below diskReserveBytes() it sets error, so the caller stops
    // the recording while the file can still be finalized.
    Status status() const;
    // Lines describing the chosen encoders etc. (for the log).
    const std::vector<std::string>& log() const { return log_; }

private:
    void onAudio(const std::vector<std::span<const float>>& streams, std::uint32_t frames);

    RecordSettings settings_;
    std::string path_;
    std::string shmSuffix_;
    std::vector<std::string> log_;

    std::unique_ptr<Muxer> muxer_;
    std::vector<std::unique_ptr<AudioEncoder>> audioEncoders_;
    std::vector<int> audioStreams_;
    std::unique_ptr<AudioCapture> audio_;
    mutable std::mutex audioErrorMutex_;
    std::string audioError_;

#ifdef _WIN32
    VideoMix* mix_ = nullptr;
    const void* videoDevice_ = nullptr;  // the mix's device when recording started (frames of another are skipped)
    std::unique_ptr<VideoEncoder> videoEncoder_;
    std::unique_ptr<Converter> converter_;
    int videoStream_ = -1;
    int consumerId_ = 0;
    std::int64_t lastPts_ = -1;
    std::atomic<std::uint64_t> videoFrames_{0};
#endif

    FreeSpaceProbe freeSpace_;
    std::uint64_t reserveBytes_ = 0;
    mutable std::mutex diskMutex_;
    mutable std::int64_t nextDiskCheckQpc_ = 0;
    mutable std::uint64_t diskFree_ = 0;
    mutable std::string diskError_;

    std::int64_t startQpc_ = 0;
    std::int64_t stopQpc_ = 0;
    bool running_ = false;
};

}  // namespace dc::capd
