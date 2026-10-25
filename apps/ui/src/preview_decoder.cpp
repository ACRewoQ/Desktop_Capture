#include "preview_decoder.hpp"

#include "dc/mfvideo/mf_codec.hpp"

#include <exception>
#include <iostream>

namespace dc::ui {

namespace {

// More undecoded units than this = the decoder cannot keep up: drop them and start again at a keyframe.
constexpr std::size_t kMaxQueue = 30;
// Decoder errors in a row before the hardware decoder is given up for the software one.
constexpr int kMaxHardwareFailures = 3;

} // namespace

PreviewDecoder::PreviewDecoder(const LUID& adapter, std::function<void()> onFrame) : onFrame_(std::move(onFrame)) {
    thread_ = std::thread([this, adapter] { run(adapter); });
}

PreviewDecoder::~PreviewDecoder() {
    {
        std::lock_guard lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void PreviewDecoder::push(std::span<const std::uint8_t> accessUnit, bool keyframe, std::uint64_t number) {
    {
        std::lock_guard lock(mu_);
        if (stop_) return;
        if (queue_.size() >= kMaxQueue) {
            queue_.clear();
            waitKey_ = true;
        }
        if (waitKey_ && !keyframe) return;
        waitKey_ = false;
        queue_.push_back({std::vector<std::uint8_t>(accessUnit.begin(), accessUnit.end()), keyframe, number});
    }
    cv_.notify_one();
}

std::shared_ptr<v6::SharedFrameRing> PreviewDecoder::ring() const {
    std::lock_guard lock(mu_);
    return ring_;
}

std::string PreviewDecoder::decoderName() const {
    std::lock_guard lock(mu_);
    return name_;
}

std::string PreviewDecoder::error() const {
    std::lock_guard lock(mu_);
    return error_;
}

void PreviewDecoder::openDecoder(bool hardware) {
    decoder_.reset();
    // The size and rate only seed the input type; the decoder follows the SPS of the stream.
    decoder_ = std::make_unique<v6::H264Decoder>(hardware ? device_.Get() : nullptr, 960, 540, 30);
    hardware_ = decoder_->hardware();
    std::lock_guard lock(mu_);
    name_ = decoder_->name() + (hardware_ ? " [D3D11/DXVA]" : " [software]");
}

void PreviewDecoder::run(LUID adapter) {
    // COM (multithreaded apartment) and Media Foundation for this thread.
    std::unique_ptr<v6::MfRuntime> mf;
    try {
        mf = std::make_unique<v6::MfRuntime>();
        device_ = v6::createVideoDevice(&adapter);
        try {
            openDecoder(true);
        } catch (const std::exception& e) {
            std::cerr << "preview: hardware H.264 decoder unavailable (" << e.what() << "), using software" << std::endl;
            openDecoder(false);
        }
    } catch (const std::exception& e) {
        std::lock_guard lock(mu_);
        error_ = e.what();
        return;
    }

    for (;;) {
        Unit unit;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) break;
            unit = std::move(queue_.front());
            queue_.pop_front();
        }
        v6::EncodedUnit u;
        u.id = unit.number;
        u.keyframe = unit.keyframe;
        u.data = std::move(unit.data);
        try {
            decoder_->decode(u, [&](IMFSample* sample, std::uint64_t, const v6::VideoInfo& info) { publish(sample, info); });
            failures_ = 0;
        } catch (const std::exception& e) {
            std::cerr << "preview: decoding failed: " << e.what() << std::endl;
            const bool toSoftware = hardware_ && ++failures_ >= kMaxHardwareFailures;
            {
                std::lock_guard lock(mu_);
                queue_.clear();
                waitKey_ = true;  // a fresh decoder starts at a keyframe
            }
            try {
                openDecoder(hardware_ && !toSoftware);
            } catch (const std::exception& e2) {
                std::lock_guard lock(mu_);
                error_ = e2.what();
                break;
            }
        }
    }
    decoder_.reset();
    converter_.reset();
}

void PreviewDecoder::publish(IMFSample* sample, const v6::VideoInfo& info) {
    LONG w = info.visible.right - info.visible.left, h = info.visible.bottom - info.visible.top;
    if (w <= 0 || h <= 0) {
        w = static_cast<LONG>(info.codedWidth);
        h = static_cast<LONG>(info.codedHeight);
    }
    std::shared_ptr<v6::SharedFrameRing> ring = this->ring();
    if (!ring || ring->width() != w || ring->height() != h) {
        // A new size (first frame, or /video/preview/width|height changed): new ring and converter.
        ring = std::make_shared<v6::SharedFrameRing>(device_.Get(), w, h);
        converter_ = std::make_unique<v6::FrameConverter>(device_.Get(), w, h);
        std::lock_guard lock(mu_);
        ring_ = ring;
    }
    ID3D11Texture2D* target = ring->beginWrite();
    converter_->convert(sample, info, target);
    ring->endWrite(decoded_.fetch_add(1, std::memory_order_relaxed) + 1);
    if (onFrame_) onFrame_();
}

} // namespace dc::ui
