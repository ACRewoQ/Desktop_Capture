#pragma once

#include "dc/clocksync/audio_fifo.hpp"
#include "dc/lanmedia/audio_codec.hpp"
#include "dc/lanmedia/packet.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace dc::lan {

// Sending end of one audio stream ("LAN送信" output of a track or the master).
//
// The engine's audio thread hands over blocks of any size with push(); a network thread calls pump(),
// which cuts them into packets of the configured size, encodes them and passes each datagram to the
// sink (which sends it to every subscribed receiver). Packets carry a sequence number, the source
// frame position and the sender time of their first frame.
//
// Threads: push() on the audio thread (real-time safe), pump() and onReport() on one network thread,
// stats() anywhere.
class AudioSender {
public:
    struct Config {
        std::uint32_t streamId = 0; // 0 = pick a random one
        AudioEncoder::Config encoder{};
        double nominalRate = 48000;
        std::size_t fifoFrames = 16384;
    };

    struct Stats {
        std::uint64_t packets = 0;
        std::uint64_t bytes = 0;   // datagram bytes
        std::uint64_t overruns = 0; // push() found the FIFO full (pump() not called often enough)
        std::uint64_t encodeErrors = 0;
        // from the newest receiver report
        bool haveReport = false;
        Report report{};
        double rttMs = 0; // round trip estimated from the report's echo
    };

    using Sink = std::function<void(std::span<const std::uint8_t>)>;

    explicit AudioSender(Config config);

    // Audio thread: interleaved frames (encoder.channels), and the time their first frame was taken,
    // in seconds on the sender's clock (QPC).
    void push(const float* frames, std::size_t count, double timeOfFirstFrame);

    // Network thread: sends every complete packet. Returns the number sent.
    std::size_t pump(const Sink& sink);

    // Network thread: a receiver report for this stream arrived at `now` (sender clock, seconds).
    void onReport(const Report& report, double now);

    std::uint32_t streamId() const { return streamId_; }
    const Config& config() const { return cfg_; }
    Stats stats() const;

private:
    struct Stamp {
        std::uint64_t pos;
        double time;
    };

    Config cfg_;
    std::uint32_t streamId_;
    clocksync::AudioFifo fifo_;
    clocksync::SpscQueue<Stamp, 256> stamps_;
    std::atomic<std::uint64_t> overruns_{0};

    // network thread
    AudioEncoder encoder_;
    std::vector<float> block_;
    std::array<std::uint8_t, kMaxDatagramBytes> datagram_{};
    bool haveStamp_ = false;
    Stamp stamp_{};
    std::uint32_t seq_ = 0;
    bool first_ = true;
    std::atomic<std::uint64_t> packets_{0}, bytes_{0}, encodeErrors_{0};
    std::atomic<bool> haveReport_{false};
    Report report_{};
    double rttMs_ = 0;
    mutable std::atomic_flag reportLock_ = ATOMIC_FLAG_INIT;
};

// A random non-zero stream id.
std::uint32_t randomStreamId();

} // namespace dc::lan
