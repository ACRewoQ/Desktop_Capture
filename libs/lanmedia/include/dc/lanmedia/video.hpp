#pragma once

#include "dc/lanmedia/arrival_clock.hpp"
#include "dc/lanmedia/packet.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace dc::lan {

// Sending end of one video stream (remote preview, or the "LAN送信" output of a video mix).
//
// The encoder hands over one access unit (H.264 Annex B, SPS/PPS in front of every IDR) at a time;
// the sender cuts it into fragments that fit a datagram and passes each one to the sink. Every
// fragment carries the frame number (media position), the capture time and the keyframe flag.
// Nothing is retransmitted: a receiver that lost part of a frame asks for a keyframe in its report,
// and the encoder thread polls takeKeyframeRequest() before each frame.
//
// Threads: send() on the encoder thread, onReport() on the network thread, the rest anywhere.
class VideoSender {
public:
    struct Config {
        std::uint32_t streamId = 0;       // 0 = pick a random one
        double keyframeHoldSeconds = 0.05; // requests this soon after a keyframe was sent are answered by it
    };

    struct Stats {
        std::uint64_t frames = 0;
        std::uint64_t keyframes = 0;
        std::uint64_t datagrams = 0;
        std::uint64_t bytes = 0;            // datagram bytes
        std::uint64_t keyframeRequests = 0; // reports that asked for a keyframe
        std::uint64_t tooLarge = 0;         // access units over the 65535 fragment limit (dropped)
        bool haveReport = false;
        Report report{};
        double rttMs = 0;
    };

    using Sink = std::function<void(std::span<const std::uint8_t>)>;

    VideoSender() : VideoSender(Config{}) {}
    explicit VideoSender(Config config);

    // Encoder thread: one access unit, the capture time of the frame (sender clock, seconds), and
    // now (seconds, same clock) for the keyframe bookkeeping. Returns datagrams sent.
    std::size_t send(std::span<const std::uint8_t> accessUnit, bool keyframe, double captureTime, double now,
                     const Sink& sink);

    // Encoder thread, before encoding the next frame: true when a receiver asked for a keyframe that
    // has not been answered yet. The flag clears when a keyframe is sent.
    bool keyframeWanted() const { return wanted_.load(std::memory_order_acquire); }

    // Network thread: a receiver report for this stream (sender clock, seconds).
    void onReport(const Report& report, double now);

    // Starts a new stream (new id, frame numbers from 0): the encoder was recreated.
    void restart();

    std::uint32_t streamId() const { return streamId_.load(std::memory_order_relaxed); }
    Stats stats() const;

private:
    Config cfg_;
    std::atomic<std::uint32_t> streamId_;
    std::atomic<bool> wanted_{false};
    std::atomic<double> lastKeyframe_{-1e9};

    // encoder thread
    std::uint64_t frame_ = 0;
    std::uint32_t seq_ = 0;
    bool first_ = true;
    std::vector<std::uint8_t> datagram_;

    std::atomic<std::uint64_t> frames_{0}, keyframes_{0}, datagrams_{0}, bytes_{0}, requests_{0}, tooLarge_{0};
    std::atomic<bool> haveReport_{false};
    Report report_{};
    double rttMs_ = 0;
    mutable std::atomic_flag reportLock_ = ATOMIC_FLAG_INIT;
};

// Receiving end of one video stream: reassembles fragments into access units and hands them over in
// order, never a frame that cannot be decoded. A frame that does not complete (lost fragment) makes the
// receiver drop frames until the next keyframe and ask the sender for one; the request is repeated
// until a keyframe arrives. The first frame delivered is always a keyframe (a viewer joining in the
// middle of a GOP asks for one right away).
//
// A frame is given up when it is still incomplete a little longer than the recent arrival lateness
// peak (Wi-Fi retries delay single datagrams by tens of ms) after its first fragment arrived, or after
// a later frame completed (the whole frame was lost).
//
// Single-threaded: onPacket() and poll() on the network thread; stats() anywhere.
class VideoReceiver {
public:
    struct Config {
        std::uint32_t streamId = 0;       // 0 = take the first stream; a StreamStart packet switches streams
        double reorderSeconds = 0.01;     // how long a gap may stay open once a later frame is complete:
        double maxReorderSeconds = 0.15;  // at least this, at most max, otherwise the arrival lateness peak + 5 ms
        double keyframeRetrySeconds = 0.1;
        double reportSeconds = 1.0;
        std::size_t maxFrameBytes = 8u << 20;
        ArrivalClock::Config arrival{1e6, 2, 20}; // positions are sender microseconds
    };

    struct Frame {
        std::span<const std::uint8_t> data; // valid during the callback
        std::uint64_t number = 0;           // frame number on the sender (consecutive within a stream)
        std::uint32_t streamId = 0;
        bool keyframe = false;
        double captureTime = 0;             // sender clock, seconds
        double arrival = 0;                 // receiver clock, seconds (last fragment)
        double lateness = 0;                // later than the fastest path, seconds
    };
    using FrameHandler = std::function<void(const Frame&)>;

    struct Stats {
        std::uint32_t streamId = 0;
        std::uint64_t datagrams = 0;
        std::uint64_t duplicates = 0;
        std::uint64_t lostDatagrams = 0;  // sequence gaps
        std::uint64_t frames = 0;         // delivered
        std::uint64_t keyframes = 0;      // delivered keyframes
        std::uint64_t droppedFrames = 0;  // incomplete, or skipped while waiting for a keyframe
        std::uint64_t lateFragments = 0;  // for a frame already delivered or given up
        std::uint64_t keyframeRequests = 0;
        std::uint64_t streamChanges = 0;
        std::uint64_t rejected = 0;       // malformed, other streams, oversized
        bool waitingForKeyframe = true;
        double latenessPeakMs = 0;
    };

    VideoReceiver() : VideoReceiver(Config{}) {}
    explicit VideoReceiver(Config config);

    // A datagram arrived at `arrival` (receiver clock, seconds). Complete frames that are next in line
    // are delivered through `out` before this returns.
    bool onPacket(std::span<const std::uint8_t> datagram, double arrival, const FrameHandler& out);

    // Call regularly (every few ms) and after onPacket(): gives up gaps whose time ran out (delivering
    // what follows, or waiting for a keyframe) and returns a report when one is due. A keyframe request
    // is sent right away and repeated every keyframeRetrySeconds.
    std::optional<Report> poll(double now, const FrameHandler& out);

    Stats stats() const;

private:
    struct Slot {
        bool used = false;
        std::uint64_t number = 0;
        std::uint16_t fragCount = 0;
        std::uint16_t fragsSeen = 0;
        std::uint32_t frameBytes = 0;
        bool keyframe = false;
        bool complete = false;
        double captureTime = 0;
        double firstArrival = 0;
        double completedAt = 0;
        double lateness = 0;
        std::vector<bool> have;
        std::vector<std::uint8_t> data;
    };
    static constexpr std::size_t kSlots = 64;

    void beginStream(std::uint32_t id);
    Slot* slotFor(std::uint64_t number, bool create);
    void deliverReady(const FrameHandler& out);
    void deliver(Slot& s, const FrameHandler& out);
    void dropBefore(std::uint64_t number);
    void needKeyframe(double now);
    void publish();

    Config cfg_;
    std::vector<Slot> slots_;
    ArrivalClock arrivalClock_;
    std::uint32_t streamId_ = 0;
    bool haveStream_ = false;
    bool waitingKey_ = true;
    bool haveNext_ = false;
    std::uint64_t next_ = 0; // next frame number to deliver (valid when haveNext_)
    double lastReport_ = -1e9, lastRequest_ = -1e9;
    bool requestPending_ = false;
    std::uint64_t echoTimeUs_ = 0;
    double echoArrival_ = 0;
    double lastArrival_ = 0;

    bool haveSeq_ = false;
    SeqUnwrapper seq_;
    std::uint64_t firstSeq_ = 0, maxSeq_ = 0;

    Stats st_;
    mutable std::atomic_flag statsLock_ = ATOMIC_FLAG_INIT;
    Stats published_;
};

} // namespace dc::lan
