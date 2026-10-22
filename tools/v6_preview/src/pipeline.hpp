// Sender (test source + encoder) and receiver (decoder + converter + shared texture) halves of the V6
// pipeline, plus a minimal TCP link so the receiver can run on another PC (e.g. the 4 GB remote device).
#pragma once

#include "d3d_video.hpp"
#include "mf_codec.hpp"
#include "stats.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace v6 {

struct PipelineOptions {
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int kbps = 8000;
    CodecKind encoder = CodecKind::Auto;
    bool hwDecode = true;
};

class SenderPipeline {
public:
    using Sink = std::function<void(EncodedUnit&&)>;
    SenderPipeline(const PipelineOptions& o, Stats& stats, Sink sink);
    ~SenderPipeline();
    const std::string& encoderName() const { return encoder_->name(); }
    bool encoderIsHardware() const { return encoder_->hardware(); }
    void start();
    void stop();

private:
    void run();
    PipelineOptions o_;
    Stats& stats_;
    Sink sink_;
    std::unique_ptr<H264Encoder> encoder_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

class ReceiverPipeline {
public:
    // onPublished(id) runs on the decode thread after a frame is available in the shared ring.
    ReceiverPipeline(const PipelineOptions& o, Stats& stats, const LUID* adapter,
                     std::function<void(std::uint64_t)> onPublished);
    ~ReceiverPipeline();

    void push(EncodedUnit&& unit);  // thread safe; stamps Received
    void stop();

    SharedFrameRing& ring() { return *ring_; }
    const std::string& decoderName() const { return decoderName_; }
    const std::string& adapterName() const { return adapterName_; }
    std::uint64_t decodedFrames() const { return decoded_.load(); }
    std::string error() const;

private:
    void run();
    PipelineOptions o_;
    Stats& stats_;
    std::function<void(std::uint64_t)> onPublished_;
    ComPtr<ID3D11Device> device_;
    std::unique_ptr<H264Decoder> decoder_;
    std::unique_ptr<FrameConverter> converter_;
    std::unique_ptr<SharedFrameRing> ring_;
    std::string decoderName_, adapterName_;

    std::thread thread_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<EncodedUnit> queue_;
    bool stop_ = false;
    std::string error_;
    std::atomic<std::uint64_t> decoded_{0};
};

// Durations the receiver reports back for each presented frame (microseconds).
struct AckTimes {
    std::uint64_t id = 0;
    std::int64_t decode = -1, convert = -1, copy = -1, present = -1;
};

// Sender side of the TCP link: sends access units, receives acks.
class TcpSender {
public:
    TcpSender(const std::string& host, int port, std::function<void(const AckTimes&)> onAck);
    ~TcpSender();
    void send(const EncodedUnit& unit);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Receiver side: accepts one connection, delivers access units, sends acks.
class TcpReceiver {
public:
    TcpReceiver(int port, std::function<void(EncodedUnit&&)> onUnit);
    ~TcpReceiver();
    void sendAck(const AckTimes& a);
    bool connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace v6
