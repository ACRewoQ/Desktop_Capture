#pragma once

// A video stream on a real socket, for the processes at both ends (docs/spec/lan-media.md 7):
//
//   VideoOutput  capd (and test senders): one encoded stream to the current subscribers, keyframe
//                requests and reports from them on the same socket (UDP 47811).
//   VideoInput   a viewer (remote preview): its own socket, reassembly, reports back to the sender.

#include "dc/lanmedia/udp_transport.hpp"
#include "dc/lanmedia/video.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dc::lan {

class VideoOutput {
public:
    struct Options {
        std::string bindAddress = "0.0.0.0";
        std::uint16_t port = kDefaultVideoPort; // 0 = any free port (tests)
        VideoSender::Config sender{};
    };

    explicit VideoOutput(Options options);
    ~VideoOutput();
    VideoOutput(const VideoOutput&) = delete;
    VideoOutput& operator=(const VideoOutput&) = delete;

    // Any thread: where the frames go (the subscribers of the stream). Empty = nobody watches; the
    // encoder can then pause.
    void setDestinations(std::vector<UdpTransport::Endpoint> to);
    bool hasDestinations() const { return destinations_.load(std::memory_order_acquire) > 0; }

    // Encoder thread.
    bool keyframeWanted() const { return sender_.keyframeWanted(); }
    std::size_t send(std::span<const std::uint8_t> accessUnit, bool keyframe, double captureTime);
    void restart() { sender_.restart(); } // the encoder was recreated: a new stream id, frames from 0

    std::uint32_t streamId() const { return sender_.streamId(); }
    std::uint16_t localPort() const { return transport_.localPort(); }
    VideoSender::Stats stats() const { return sender_.stats(); }

private:
    UdpTransport transport_;
    VideoSender sender_;
    mutable std::mutex mutex_;
    std::vector<UdpTransport::Endpoint> to_;
    std::atomic<std::size_t> destinations_{0};
};

class VideoInput {
public:
    struct Options {
        std::string bindAddress = "0.0.0.0";
        std::uint16_t port = 0; // 0 = any free port: tell the sender with /lan/subscribe
        VideoReceiver::Config receiver{};
    };
    // Called on the input's threads (never two at once) with each decodable access unit in order.
    using FrameHandler = VideoReceiver::FrameHandler;

    VideoInput(Options options, FrameHandler onFrame);
    ~VideoInput();
    VideoInput(const VideoInput&) = delete;
    VideoInput& operator=(const VideoInput&) = delete;

    std::uint16_t localPort() const { return transport_.localPort(); }
    VideoReceiver::Stats stats() const { return receiver_.stats(); }

private:
    void pollLoop();

    UdpTransport transport_;
    VideoReceiver receiver_;
    FrameHandler onFrame_;
    std::mutex mutex_; // the receiver is single-threaded: socket thread and poll thread take turns
    bool haveSource_ = false;
    UdpTransport::Endpoint source_;
    std::atomic<bool> running_{true};
    std::thread poll_;
};

} // namespace dc::lan
