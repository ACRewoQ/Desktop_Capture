#include "dc/lanmedia/video_link.hpp"

#include <chrono>

namespace dc::lan {

namespace {
UdpTransport::Options transportOptions(const std::string& bind, std::uint16_t port) {
    UdpTransport::Options o;
    o.bindAddress = bind;
    o.port = port;
    o.sendBufferBytes = 4 << 20; // a 1080p keyframe is a burst of a few hundred datagrams
    o.receiveBufferBytes = 4 << 20;
    return o;
}
} // namespace

VideoOutput::VideoOutput(Options options)
    : transport_(transportOptions(options.bindAddress, options.port)), sender_(options.sender) {
    transport_.start([this](std::span<const std::uint8_t> d, const UdpTransport::Endpoint&, double arrival) {
        if (peekType(d) != PacketType::Report) return;
        if (const auto r = parseReport(d); r && r->streamId == sender_.streamId()) sender_.onReport(*r, arrival);
    });
}

VideoOutput::~VideoOutput() { transport_.stop(); }

void VideoOutput::setDestinations(std::vector<UdpTransport::Endpoint> to) {
    std::lock_guard lock(mutex_);
    to_ = std::move(to);
    destinations_.store(to_.size(), std::memory_order_release);
}

std::size_t VideoOutput::send(std::span<const std::uint8_t> accessUnit, bool keyframe, double captureTime) {
    std::vector<UdpTransport::Endpoint> to;
    {
        std::lock_guard lock(mutex_);
        to = to_;
    }
    return sender_.send(accessUnit, keyframe, captureTime, nowSeconds(), [&](std::span<const std::uint8_t> d) {
        for (const auto& ep : to) transport_.send(d, ep);
    });
}

VideoInput::VideoInput(Options options, FrameHandler onFrame)
    : transport_(transportOptions(options.bindAddress, options.port)), receiver_(options.receiver),
      onFrame_(std::move(onFrame)) {
    transport_.start([this](std::span<const std::uint8_t> d, const UdpTransport::Endpoint& from, double arrival) {
        std::lock_guard lock(mutex_);
        if (receiver_.onPacket(d, arrival, onFrame_)) {
            source_ = from;
            haveSource_ = true;
        }
    });
    poll_ = std::thread([this] { pollLoop(); });
}

VideoInput::~VideoInput() {
    running_ = false;
    if (poll_.joinable()) poll_.join();
    transport_.stop();
}

void VideoInput::setExpectedSource(const UdpTransport::Endpoint& sender) {
    std::lock_guard lock(mutex_);
    expected_ = sender;
    haveExpected_ = true;
    lastHello_ = -1e9;
}

void VideoInput::pollLoop() {
    while (running_) {
        {
            std::lock_guard lock(mutex_);
            const double now = nowSeconds();
            if (const auto r = receiver_.poll(now, onFrame_); r && haveSource_)
                transport_.send(encodeReport(*r), source_);
            if (!haveSource_ && haveExpected_ && now - lastHello_ >= 0.5) {
                transport_.send(encodeReport(Report{}), expected_);
                lastHello_ = now;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace dc::lan
