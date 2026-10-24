#include "dc/lanmedia/audio_sender.hpp"
#include "dc/lanmedia/video.hpp"

#include <algorithm>
#include <cmath>

namespace dc::lan {

VideoSender::VideoSender(Config config)
    : cfg_(config), streamId_(config.streamId ? config.streamId : randomStreamId()), datagram_(kMaxDatagramBytes) {}

void VideoSender::restart() {
    streamId_.store(randomStreamId(), std::memory_order_relaxed);
    frame_ = 0;
    first_ = true;
    wanted_.store(false, std::memory_order_release);
}

std::size_t VideoSender::send(std::span<const std::uint8_t> au, bool keyframe, double captureTime, double now,
                              const Sink& sink) {
    if (au.empty()) return 0;
    const std::size_t count = (au.size() + kMaxVideoFragmentBytes - 1) / kMaxVideoFragmentBytes;
    if (count > 0xFFFF || au.size() > 0xFFFFFFFFu) {
        tooLarge_.fetch_add(1, std::memory_order_relaxed);
        ++frame_; // the receiver sees a gap and asks for a keyframe
        return 0;
    }
    if (keyframe) {
        lastKeyframe_.store(now, std::memory_order_relaxed);
        wanted_.store(false, std::memory_order_release);
    }

    MediaHeader h;
    h.kind = MediaKind::Video;
    h.streamId = streamId_.load(std::memory_order_relaxed);
    h.timeUs = static_cast<std::uint64_t>(std::llround(std::max(captureTime, 0.0) * 1e6));
    h.position = frame_;
    h.fragCount = static_cast<std::uint16_t>(count);
    VideoHeader v;
    v.frameBytes = static_cast<std::uint32_t>(au.size());

    std::size_t bytes = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t off = i * kMaxVideoFragmentBytes;
        const std::size_t n = std::min(kMaxVideoFragmentBytes, au.size() - off);
        h.flags = static_cast<std::uint8_t>((first_ ? kFlagStreamStart : 0) | (keyframe ? kFlagKeyframe : 0));
        h.seq = seq_++;
        h.fragIndex = static_cast<std::uint16_t>(i);
        v.offset = static_cast<std::uint32_t>(off);
        std::uint8_t* p = datagram_.data();
        writeMediaHeader(p, h);
        writeVideoHeader(p + kMediaHeaderBytes, v);
        std::copy_n(au.data() + off, n, p + kMediaHeaderBytes + kVideoHeaderBytes);
        const std::size_t total = kMediaHeaderBytes + kVideoHeaderBytes + n;
        sink(std::span<const std::uint8_t>(p, total));
        bytes += total;
    }
    first_ = false;
    ++frame_;
    frames_.fetch_add(1, std::memory_order_relaxed);
    if (keyframe) keyframes_.fetch_add(1, std::memory_order_relaxed);
    datagrams_.fetch_add(count, std::memory_order_relaxed);
    bytes_.fetch_add(bytes, std::memory_order_relaxed);
    return count;
}

void VideoSender::onReport(const Report& report, double now) {
    if (report.streamId != streamId_.load(std::memory_order_relaxed)) return;
    if (report.flags & kReportKeyframeRequest) {
        requests_.fetch_add(1, std::memory_order_relaxed);
        // A keyframe sent a moment ago is probably still on its way to this receiver, or answered an
        // earlier copy of the same request; the receiver repeats the request if it still needs one.
        if (now - lastKeyframe_.load(std::memory_order_relaxed) >= cfg_.keyframeHoldSeconds)
            wanted_.store(true, std::memory_order_release);
    }
    double rtt = 0;
    if (report.echoTimeUs != 0) rtt = (now * 1e6 - double(report.echoTimeUs) - double(report.echoHoldUs)) / 1000.0;
    while (reportLock_.test_and_set(std::memory_order_acquire)) {
    }
    report_ = report;
    rttMs_ = rtt;
    reportLock_.clear(std::memory_order_release);
    haveReport_.store(true, std::memory_order_release);
}

VideoSender::Stats VideoSender::stats() const {
    Stats s;
    s.frames = frames_.load(std::memory_order_relaxed);
    s.keyframes = keyframes_.load(std::memory_order_relaxed);
    s.datagrams = datagrams_.load(std::memory_order_relaxed);
    s.bytes = bytes_.load(std::memory_order_relaxed);
    s.keyframeRequests = requests_.load(std::memory_order_relaxed);
    s.tooLarge = tooLarge_.load(std::memory_order_relaxed);
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
