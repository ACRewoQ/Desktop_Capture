#pragma once

// Windows only: H.264 access units of the preview stream -> Media Foundation decoder (DXVA, software
// as fallback) -> BGRA in a SharedFrameRing that PreviewVideo copies onto Qt's D3D11 device. The
// same path as the P1/V6 measurement (docs/verification/v6-preview-playback.md), on its own thread
// and its own D3D11 device on Qt's adapter.

#include "dc/mfvideo/d3d_video.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace dc::ui {

class PreviewDecoder {
public:
    // onFrame runs on the decode thread after a frame was published to ring().
    PreviewDecoder(const LUID& adapter, std::function<void()> onFrame);
    ~PreviewDecoder();
    PreviewDecoder(const PreviewDecoder&) = delete;
    PreviewDecoder& operator=(const PreviewDecoder&) = delete;

    // Any thread. Units before the first keyframe are skipped. captureTime: when the engine composed
    // the frame, on this device's clock (NaN = unknown), for the delay measurement.
    void push(std::span<const std::uint8_t> accessUnit, bool keyframe, std::uint64_t number, double captureTime);
    // The capture time of the frame the ring numbered `frameId` (SharedFrameReader::fetch), NaN if unknown.
    double captureTimeOf(std::uint64_t frameId) const;

    // The ring of the current picture size, null until the first frame. A new ring replaces it when
    // the stream's size changes; holders of the old one keep it alive.
    std::shared_ptr<v6::SharedFrameRing> ring() const;
    std::string decoderName() const;
    std::string error() const;
    std::uint64_t decodedFrames() const { return decoded_.load(std::memory_order_relaxed); }

private:
    void run(LUID adapter);
    void openDecoder(bool hardware);
    void publish(IMFSample* sample, std::uint64_t number, const v6::VideoInfo& info);

    std::function<void()> onFrame_;

    // decode thread only
    v6::ComPtr<ID3D11Device> device_;
    std::unique_ptr<v6::H264Decoder> decoder_;
    std::unique_ptr<v6::FrameConverter> converter_;
    bool hardware_ = true;
    int failures_ = 0;

    std::thread thread_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    struct Unit {
        std::vector<std::uint8_t> data;
        bool keyframe = false;
        std::uint64_t number = 0;
        double captureTime = 0;
    };
    std::deque<Unit> queue_;
    // capture times: by unit number until decoded, then by ring frame id
    static constexpr std::size_t kTimes = 64;
    struct Stamp {
        std::uint64_t id = ~std::uint64_t{0};
        double time = 0;
    };
    std::array<Stamp, kTimes> byNumber_{};
    std::array<Stamp, kTimes> byFrame_{};
    bool stop_ = false;
    bool waitKey_ = true;  // skip units until a keyframe (start, overflow, decoder error)
    std::shared_ptr<v6::SharedFrameRing> ring_;
    std::string name_, error_;
    std::atomic<std::uint64_t> decoded_{0};
};

} // namespace dc::ui
