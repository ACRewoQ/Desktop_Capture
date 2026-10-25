// LAN video output: a video mix's canvas, encoded, to the viewers of one LAN video stream (the remote preview
// "video/preview" or the "LAN送信" output "video/lan"; docs/spec/protocol.md 10, lan-media.md 7).
//
// Runs on the video thread (a consumer of the mix): at the stream's frame rate (one frame per 1/fps slot of the QPC
// clock, whatever the mix rate) the GPU converter scales the canvas into an NV12 surface of the encoder, which
// encodes on its own thread (H.264, low-latency tuning) and hands each access unit to the stream's socket. Nothing
// is encoded while nobody watches. A viewer's keyframe request (lost packets, joining) is answered with an IDR on
// the next frame.
//
// The encoder lives on the mix's device. After a GPU reset the frames come from a new device: the output marks
// itself stale and the follower creates a new one (a new stream for the viewers, starting with a keyframe).
#pragma once

#include "d3d.hpp"
#include "lan_stream.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dc::lan {
class VideoOutput;
}

namespace dc::capd {

class Converter;
class VideoEncoder;
struct MixFrame;

class LanVideoOutput {
public:
    struct Stats {
        std::uint64_t frames = 0;     // submitted to the encoder
        std::uint64_t keyframes = 0;  // forced: first frame, after a pause, viewer requests
        std::uint64_t poolEmpty = 0;  // dropped: every encoder surface still in use
    };

    // Opens the encoder on dev (throws when none opens). socket: the stream's socket, kept by the caller for longer
    // than this output (its viewers stay subscribed when the output is recreated).
    LanVideoOutput(lan::VideoOutput& socket, const LanStreamConfig& config, const Device& dev);
    ~LanVideoOutput();
    LanVideoOutput(const LanVideoOutput&) = delete;
    LanVideoOutput& operator=(const LanVideoOutput&) = delete;

    void onFrame(const MixFrame& f);  // video thread

    // The frames come from another device (GPU reset) or the encoder failed: the follower replaces this output.
    bool stale() const;
    std::string error() const;  // why it is stale (empty after a GPU reset)
    std::string description() const { return description_; }
    const std::vector<std::string>& log() const { return log_; }
    Stats stats() const;

private:
    lan::VideoOutput& socket_;
    LanStreamConfig config_;
    Device dev_;
    std::unique_ptr<VideoEncoder> encoder_;
    std::unique_ptr<Converter> converter_;
    std::string description_;
    std::vector<std::string> log_;

    // video thread
    std::int64_t firstSlot_ = -1, lastSlot_ = -1;
    bool paused_ = true;  // nobody was watching: the next frame is a keyframe

    // encoder thread
    ParameterSets parameterSets_;

    // capture time of each frame in flight, by pts (the encoder keeps far fewer than this in flight)
    static constexpr std::size_t kInFlight = 64;
    std::array<std::atomic<std::int64_t>, kInFlight> qpcOf_{};

    std::atomic<bool> stale_{false};
    std::atomic<std::uint64_t> frames_{0}, keyframes_{0};
};

}  // namespace dc::capd
