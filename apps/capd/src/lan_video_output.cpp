#include "lan_video_output.hpp"

#include "converter.hpp"
#include "video_encoder.hpp"
#include "video_mix.hpp"

#include "dc/lanmedia/video_link.hpp"
#include "dc/shmaudio/clock.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace dc::capd {

LanVideoOutput::LanVideoOutput(lan::VideoOutput& socket, const LanStreamConfig& config, const Device& dev)
    : socket_(socket), config_(config), dev_(dev) {
    const VideoSettings v = lanVideoSettings(config_);
    // No global header: SPS/PPS go in front of every IDR, where a viewer can start.
    encoder_ = openVideoEncoder(dev_, v, false, &log_);
    converter_ = std::make_unique<Converter>(dev_, UINT(v.width), UINT(v.height), UINT(v.fps));
    description_ = encoder_->description();
    socket_.restart();  // a new stream: viewers switch to it at the first keyframe
    const std::int64_t freq = shm::qpcFrequency();
    encoder_->start([this, freq](AVPacket* p) {
        const bool key = (p->flags & AV_PKT_FLAG_KEY) != 0;
        const auto au = parameterSets_.process({p->data, std::size_t(p->size)}, key);
        const std::int64_t qpc = p->pts >= 0 ? qpcOf_[std::size_t(p->pts) % kInFlight].load() : shm::nowQpc();
        socket_.send(au, key, double(qpc) / double(freq));
    });
}

LanVideoOutput::~LanVideoOutput() {
    if (encoder_) encoder_->finish();
}

void LanVideoOutput::onFrame(const MixFrame& f) {
    if (f.device->device.Get() != dev_.device.Get()) {
        stale_.store(true, std::memory_order_release);
        return;
    }
    if (!socket_.hasDestinations()) {
        paused_ = true;
        return;
    }
    const std::int64_t slot = f.qpc * config_.fps / shm::qpcFrequency();
    if (slot == lastSlot_) return;  // a mix faster than the stream: skip ticks
    lastSlot_ = slot;
    if (firstSlot_ < 0) firstSlot_ = slot;

    auto surface = encoder_->acquire();
    if (!surface.frame) return;  // encoder behind (counted by the encoder)
    converter_->convert(f.canvas, surface.texture, surface.arrayIndex);
    const bool key = paused_ || socket_.keyframeWanted();
    paused_ = false;
    const std::int64_t pts = slot - firstSlot_;
    qpcOf_[std::size_t(pts) % kInFlight].store(f.qpc);
    encoder_->submit(surface, pts, key);
    ++frames_;
    if (key) ++keyframes_;
}

bool LanVideoOutput::stale() const { return stale_.load(std::memory_order_acquire) || !encoder_->error().empty(); }

std::string LanVideoOutput::error() const { return encoder_->error(); }

LanVideoOutput::Stats LanVideoOutput::stats() const {
    Stats s;
    s.frames = frames_.load();
    s.keyframes = keyframes_.load();
    s.poolEmpty = encoder_->stats().poolEmpty;
    return s;
}

}  // namespace dc::capd
