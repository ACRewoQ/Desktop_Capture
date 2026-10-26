#include "dc/lanmedia/audio_link.hpp"

#include <chrono>

namespace dc::lan {

namespace {
UdpTransport::Options transportOptions(const std::string& bind, std::uint16_t port) {
    UdpTransport::Options o;
    o.bindAddress = bind;
    o.port = port;
    return o;
}
constexpr double kReportSeconds = 1.0; // lan-media.md 2.4
constexpr double kKnockSeconds = 0.5;
} // namespace

AudioInput::AudioInput(Options options)
    : transport_(transportOptions(options.bindAddress, options.port)), receiver_(options.receiver) {
    transport_.start([this](std::span<const std::uint8_t> d, const UdpTransport::Endpoint& from, double arrival) {
        if (!receiver_.onPacket(d, arrival)) return;
        std::lock_guard lock(mutex_);
        source_ = from;
        haveSource_ = true;
    });
    reports_ = std::thread([this] { reportLoop(); });
}

AudioInput::~AudioInput() {
    running_ = false;
    if (reports_.joinable()) reports_.join();
    transport_.stop();
}

void AudioInput::setExpectedSource(const UdpTransport::Endpoint& sender) {
    std::lock_guard lock(mutex_);
    expected_ = sender;
    haveExpected_ = true;
}

void AudioInput::reportLoop() {
    double next = 0;
    while (running_) {
        const double now = nowSeconds();
        if (now >= next) {
            std::lock_guard lock(mutex_);
            if (haveSource_) {
                transport_.send(encodeReport(receiver_.makeReport(now)), source_);
                next = now + kReportSeconds;
            } else if (haveExpected_) {
                transport_.send(encodeReport(Report{}), expected_);
                next = now + kKnockSeconds;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace dc::lan
