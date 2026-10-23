#include "audio_encoder.hpp"

#include "muxer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dc::capd {

namespace {

constexpr int kRate = 48000;

void check(int err, const std::string& what) {
    if (err < 0) throw std::runtime_error(what + ": " + avError(err));
}

}  // namespace

AudioEncoder::AudioEncoder(const AudioStreamSettings& settings, bool globalHeader) : settings_(settings) {
    const AVCodec* codec = nullptr;
    AVSampleFormat fmt = AV_SAMPLE_FMT_FLTP;
    if (settings_.codec == "aac") {
        codec = avcodec_find_encoder_by_name("aac");
    } else if (settings_.codec == "opus") {
        codec = avcodec_find_encoder_by_name("libopus");
        if (codec) fmt = AV_SAMPLE_FMT_FLT;  // libopus takes interleaved float
        else codec = avcodec_find_encoder_by_name("opus");  // FFmpeg's own (experimental) Opus encoder
    } else if (settings_.codec == "pcm") {
        codec = avcodec_find_encoder_by_name("pcm_s24le");
        fmt = AV_SAMPLE_FMT_S32;
    } else {
        throw std::runtime_error("unknown audio codec " + settings_.codec);
    }
    if (!codec) throw std::runtime_error("audio encoder for " + settings_.codec + " is not in this FFmpeg build");

    ctx_ = avcodec_alloc_context3(codec);
    ctx_->sample_rate = kRate;
    ctx_->sample_fmt = fmt;
    ctx_->time_base = AVRational{1, kRate};
    av_channel_layout_default(&ctx_->ch_layout, 2);
    if (settings_.codec != "pcm") ctx_->bit_rate = std::int64_t{settings_.bitrateKbps} * 1000;
    if (std::string(codec->name) == "opus") ctx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    if (globalHeader) ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    check(avcodec_open2(ctx_, codec, nullptr), "open audio encoder " + std::string(codec->name));

    // PCM has no frame size of its own: 10 ms packets.
    frameSize_ = ctx_->frame_size > 0 ? static_cast<std::uint32_t>(ctx_->frame_size) : 480;

    frame_ = av_frame_alloc();
    frame_->format = ctx_->sample_fmt;
    frame_->sample_rate = kRate;
    check(av_channel_layout_copy(&frame_->ch_layout, &ctx_->ch_layout), "channel layout");
    frame_->nb_samples = static_cast<int>(frameSize_);
    check(av_frame_get_buffer(frame_, 0), "audio frame buffer");
    pkt_ = av_packet_alloc();
    pending_.reserve(frameSize_ * 4);
}

AudioEncoder::~AudioEncoder() {
    av_packet_free(&pkt_);
    av_frame_free(&frame_);
    avcodec_free_context(&ctx_);
}

std::string AudioEncoder::description() const {
    std::string d = std::string(ctx_->codec->name);
    if (settings_.codec != "pcm") d += " " + std::to_string(settings_.bitrateKbps) + " kbps";
    return d;
}

void AudioEncoder::push(std::span<const float> stereo, std::uint32_t frames) {
    pending_.insert(pending_.end(), stereo.begin(), stereo.begin() + std::ptrdiff_t{frames} * 2);
    std::size_t used = 0;
    while (pending_.size() - used >= std::size_t{frameSize_} * 2) {
        encodeFrame(pending_.data() + used, frameSize_);
        used += std::size_t{frameSize_} * 2;
    }
    if (used) pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(used));
}

void AudioEncoder::encodeFrame(const float* src, std::uint32_t frames) {
    check(av_frame_make_writable(frame_), "audio frame writable");
    frame_->nb_samples = static_cast<int>(frameSize_);
    if (ctx_->sample_fmt == AV_SAMPLE_FMT_FLTP) {
        auto* l = reinterpret_cast<float*>(frame_->data[0]);
        auto* r = reinterpret_cast<float*>(frame_->data[1]);
        for (std::uint32_t i = 0; i < frameSize_; ++i) {
            l[i] = i < frames ? src[i * 2] : 0.0f;
            r[i] = i < frames ? src[i * 2 + 1] : 0.0f;
        }
    } else if (ctx_->sample_fmt == AV_SAMPLE_FMT_FLT) {
        auto* out = reinterpret_cast<float*>(frame_->data[0]);
        for (std::uint32_t i = 0; i < frameSize_ * 2; ++i) out[i] = i < frames * 2 ? src[i] : 0.0f;
    } else {
        // pcm_s24le takes S32 and keeps the upper 24 bits.
        auto* out = reinterpret_cast<std::int32_t*>(frame_->data[0]);
        for (std::uint32_t i = 0; i < frameSize_ * 2; ++i) {
            const float s = i < frames * 2 ? std::clamp(src[i], -1.0f, 1.0f) : 0.0f;
            out[i] = static_cast<std::int32_t>(std::lrint(static_cast<double>(s) * 2147483392.0));
        }
    }
    frame_->pts = pts_;
    pts_ += frameSize_;
    check(avcodec_send_frame(ctx_, frame_), "encode audio");
    drain();
}

void AudioEncoder::drain() {
    for (;;) {
        const int r = avcodec_receive_packet(ctx_, pkt_);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
        check(r, "audio packet");
        if (sink_) sink_(pkt_);
        av_packet_unref(pkt_);
    }
}

void AudioEncoder::finish() {
    if (finished_) return;
    finished_ = true;
    if (!pending_.empty()) encodeFrame(pending_.data(), static_cast<std::uint32_t>(pending_.size() / 2));
    pending_.clear();
    check(avcodec_send_frame(ctx_, nullptr), "flush audio");
    drain();
}

}  // namespace dc::capd
