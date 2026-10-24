#include "dc/lanmedia/audio_sender.hpp"

#include <cmath>
#include <random>

namespace dc::lan {

std::uint32_t randomStreamId() {
    static std::mt19937 rng{std::random_device{}()};
    std::uint32_t id = 0;
    while (id == 0) id = static_cast<std::uint32_t>(rng());
    return id;
}

AudioSender::AudioSender(Config config)
    : cfg_(config), streamId_(config.streamId ? config.streamId : randomStreamId()),
      fifo_(config.encoder.channels, config.fifoFrames), encoder_(config.encoder),
      block_(config.encoder.frames * std::size_t(config.encoder.channels)) {}

void AudioSender::push(const float* frames, std::size_t count, double timeOfFirstFrame) {
    const auto pos = fifo_.written();
    const auto stored = fifo_.push(frames, count);
    if (stored < count) overruns_.fetch_add(1, std::memory_order_relaxed);
    if (stored > 0) stamps_.push({pos, timeOfFirstFrame});
}

std::size_t AudioSender::pump(const Sink& sink) {
    Stamp s;
    while (stamps_.pop(s)) {
        stamp_ = s;
        haveStamp_ = true;
    }
    const std::size_t frames = cfg_.encoder.frames;
    std::size_t sent = 0;
    while (fifo_.available() >= frames) {
        const std::uint64_t pos = fifo_.read();
        fifo_.pop(block_.data(), frames);
        const double t = haveStamp_ ? stamp_.time + (double(pos) - double(stamp_.pos)) / cfg_.nominalRate : 0.0;

        std::uint8_t* p = datagram_.data();
        const std::size_t room = datagram_.size() - kMediaHeaderBytes - kAudioHeaderBytes;
        const std::size_t bytes = encoder_.encode(block_.data(), p + kMediaHeaderBytes + kAudioHeaderBytes, room);
        if (bytes == 0) {
            encodeErrors_.fetch_add(1, std::memory_order_relaxed);
            continue; // the position still advances: the receiver conceals the gap
        }
        MediaHeader h;
        h.kind = MediaKind::Audio;
        h.flags = first_ ? kFlagStreamStart : 0;
        h.streamId = streamId_;
        h.seq = seq_++;
        h.timeUs = static_cast<std::uint64_t>(std::llround(std::max(t, 0.0) * 1e6));
        h.position = pos;
        writeMediaHeader(p, h);
        writeAudioHeader(p + kMediaHeaderBytes,
                         {cfg_.encoder.codec, static_cast<std::uint8_t>(cfg_.encoder.channels),
                          static_cast<std::uint16_t>(frames)});
        const std::size_t total = kMediaHeaderBytes + kAudioHeaderBytes + bytes;
        sink(std::span<const std::uint8_t>(p, total));
        first_ = false;
        ++sent;
        packets_.fetch_add(1, std::memory_order_relaxed);
        bytes_.fetch_add(total, std::memory_order_relaxed);
    }
    return sent;
}

void AudioSender::onReport(const Report& report, double now) {
    if (report.streamId != streamId_) return;
    double rtt = 0;
    if (report.echoTimeUs != 0) rtt = (now * 1e6 - double(report.echoTimeUs) - double(report.echoHoldUs)) / 1000.0;
    while (reportLock_.test_and_set(std::memory_order_acquire)) {
    }
    report_ = report;
    rttMs_ = rtt;
    reportLock_.clear(std::memory_order_release);
    haveReport_.store(true, std::memory_order_release);
}

AudioSender::Stats AudioSender::stats() const {
    Stats s;
    s.packets = packets_.load(std::memory_order_relaxed);
    s.bytes = bytes_.load(std::memory_order_relaxed);
    s.overruns = overruns_.load(std::memory_order_relaxed);
    s.encodeErrors = encodeErrors_.load(std::memory_order_relaxed);
    s.haveReport = haveReport_.load(std::memory_order_acquire);
    if (s.haveReport) {
        while (reportLock_.test_and_set(std::memory_order_acquire)) {
        }
        s.report = report_;
        s.rttMs = rttMs_;
        reportLock_.clear(std::memory_order_release);
    }
    return s;
}

} // namespace dc::lan
