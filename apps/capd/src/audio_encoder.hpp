// One recorded audio stream: interleaved stereo float at 48 kHz in, AAC / Opus / PCM (24-bit) packets out.
// Timestamps count samples from the recording start, so the stream lines up with the video by construction.
#pragma once

#include "settings.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace dc::capd {

class AudioEncoder {
public:
    using PacketSink = std::function<void(AVPacket*)>;

    AudioEncoder(const AudioStreamSettings& settings, bool globalHeader);
    ~AudioEncoder();
    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    const AVCodecContext* context() const { return ctx_; }
    std::string description() const;  // e.g. "aac 192 kbps"

    void setSink(PacketSink sink) { sink_ = std::move(sink); }

    // Appends frames * 2 samples (L, R, L, R ...). Encodes every complete codec frame.
    void push(std::span<const float> stereo, std::uint32_t frames);

    // Encodes what is left (padded with silence) and flushes the codec.
    void finish();

private:
    void encodeFrame(const float* src, std::uint32_t frames);  // frames <= frameSize_, rest is silence
    void drain();

    AudioStreamSettings settings_;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    PacketSink sink_;
    std::uint32_t frameSize_ = 1024;
    std::vector<float> pending_;  // interleaved stereo not yet encoded
    std::int64_t pts_ = 0;
    bool finished_ = false;
};

}  // namespace dc::capd
