// One recording: video from a mix (Windows) and the audio streams from the engine's shared memory, encoded and
// written into one MKV/MP4 file. The recording's clock is QPC: time zero is the moment start() runs, video frames
// get their pts from the mix tick time and audio from the sample count since time zero, so both stay aligned.
#pragma once

#include "settings.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
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
        std::string error;  // non-empty: the recording failed and must be stopped
    };

#ifdef _WIN32
    // mix may be null for an audio-only recording.
    Recorder(const RecordSettings& s, VideoMix* mix, std::string path, std::string shmSuffix = "");
#else
    Recorder(const RecordSettings& s, std::string path, std::string shmSuffix = "");
#endif
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Opens encoders and the file and starts capturing. Throws with a readable message on failure.
    void start();
    // Stops and finalizes the file. Returns the final status (error set if finalizing failed).
    Status stop();

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
    std::unique_ptr<VideoEncoder> videoEncoder_;
    std::unique_ptr<Converter> converter_;
    int videoStream_ = -1;
    int consumerId_ = 0;
    std::int64_t lastPts_ = -1;
    std::atomic<std::uint64_t> videoFrames_{0};
#endif

    std::int64_t startQpc_ = 0;
    std::int64_t stopQpc_ = 0;
    bool running_ = false;
};

}  // namespace dc::capd
