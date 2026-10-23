#include "dc/lanmedia/audio_codec.hpp"

#include <opus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace dc::lan {

namespace {

constexpr int kRate = 48000;

std::int32_t toInt(float v, double scale) {
    const double x = std::clamp(static_cast<double>(v), -1.0, 1.0) * scale;
    return static_cast<std::int32_t>(std::lround(std::min(x, scale - 1.0)));
}

} // namespace

std::size_t pcmBytes(AudioCodec codec, int channels, std::size_t frames) {
    return frames * std::size_t(channels) * (codec == AudioCodec::Pcm16 ? 2u : 3u);
}

bool validPacketFrames(AudioCodec codec, int channels, std::size_t frames) {
    if (channels != 1 && channels != 2) return false;
    if (codec == AudioCodec::Opus) return frames == 120 || frames == 240 || frames == 480 || frames == 960;
    return frames > 0 && kAudioHeaderBytes + pcmBytes(codec, channels, frames) <= kMaxMediaPayloadBytes;
}

AudioEncoder::AudioEncoder(Config config) : cfg_(config) {
    if (!validPacketFrames(cfg_.codec, cfg_.channels, cfg_.frames))
        throw std::invalid_argument("lanmedia: packet size not supported by the codec");
    if (cfg_.codec != AudioCodec::Opus) return;
    int err = 0;
    opus_ = opus_encoder_create(kRate, cfg_.channels, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || !opus_) throw std::runtime_error(std::string("opus_encoder_create: ") + opus_strerror(err));
    opus_encoder_ctl(opus_, OPUS_SET_BITRATE(cfg_.opusBitrate));
    opus_encoder_ctl(opus_, OPUS_SET_INBAND_FEC(cfg_.opusFec ? 1 : 0));
    opus_encoder_ctl(opus_, OPUS_SET_PACKET_LOSS_PERC(cfg_.opusFec ? cfg_.opusExpectedLossPercent : 0));
    opus_encoder_ctl(opus_, OPUS_SET_COMPLEXITY(8));
}

AudioEncoder::~AudioEncoder() {
    if (opus_) opus_encoder_destroy(opus_);
}

std::size_t AudioEncoder::encode(const float* in, std::uint8_t* out, std::size_t maxBytes) {
    const std::size_t n = cfg_.frames * std::size_t(cfg_.channels);
    switch (cfg_.codec) {
    case AudioCodec::Pcm16:
        if (maxBytes < n * 2) return 0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto v = static_cast<std::uint16_t>(toInt(in[i], 32768.0));
            out[2 * i] = static_cast<std::uint8_t>(v & 0xFF);
            out[2 * i + 1] = static_cast<std::uint8_t>(v >> 8);
        }
        return n * 2;
    case AudioCodec::Pcm24:
        if (maxBytes < n * 3) return 0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto v = static_cast<std::uint32_t>(toInt(in[i], 8388608.0));
            out[3 * i] = static_cast<std::uint8_t>(v & 0xFF);
            out[3 * i + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
            out[3 * i + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        }
        return n * 3;
    case AudioCodec::Opus: {
        const auto bytes = opus_encode_float(opus_, in, static_cast<int>(cfg_.frames), out,
                                             static_cast<opus_int32>(std::min<std::size_t>(maxBytes, 1275)));
        return bytes > 0 ? std::size_t(bytes) : 0;
    }
    }
    return 0;
}

AudioDecoder::AudioDecoder(AudioCodec codec, int channels, std::size_t frames)
    : codec_(codec), channels_(channels), frames_(frames) {
    if (!validPacketFrames(codec, channels, frames))
        throw std::invalid_argument("lanmedia: packet size not supported by the codec");
    if (codec != AudioCodec::Opus) return;
    int err = 0;
    opus_ = opus_decoder_create(kRate, channels, &err);
    if (err != OPUS_OK || !opus_) throw std::runtime_error(std::string("opus_decoder_create: ") + opus_strerror(err));
}

AudioDecoder::~AudioDecoder() {
    if (opus_) opus_decoder_destroy(opus_);
}

bool AudioDecoder::decode(std::span<const std::uint8_t> data, float* out) {
    const std::size_t n = frames_ * std::size_t(channels_);
    switch (codec_) {
    case AudioCodec::Pcm16:
        if (data.size() != n * 2) return false;
        for (std::size_t i = 0; i < n; ++i) {
            const auto v = static_cast<std::int16_t>(data[2 * i] | (data[2 * i + 1] << 8));
            out[i] = static_cast<float>(v) * (1.0f / 32768.0f);
        }
        return true;
    case AudioCodec::Pcm24:
        if (data.size() != n * 3) return false;
        for (std::size_t i = 0; i < n; ++i) {
            std::int32_t v = data[3 * i] | (data[3 * i + 1] << 8) | (data[3 * i + 2] << 16);
            if (v & 0x800000) v -= 0x1000000;
            out[i] = static_cast<float>(v) * (1.0f / 8388608.0f);
        }
        return true;
    case AudioCodec::Opus: {
        const int got = opus_decode_float(opus_, data.data(), static_cast<opus_int32>(data.size()), out,
                                          static_cast<int>(frames_), 0);
        if (got == static_cast<int>(frames_)) return true;
        // a malformed packet: keep the decoder's timeline by concealing instead
        conceal(out);
        return false;
    }
    }
    return false;
}

bool AudioDecoder::decodeFec(std::span<const std::uint8_t> next, float* out) {
    if (codec_ != AudioCodec::Opus || next.empty()) return false;
    const int got = opus_decode_float(opus_, next.data(), static_cast<opus_int32>(next.size()), out,
                                      static_cast<int>(frames_), 1);
    return got == static_cast<int>(frames_);
}

bool AudioDecoder::conceal(float* out) {
    if (codec_ != AudioCodec::Opus) return false;
    const int got = opus_decode_float(opus_, nullptr, 0, out, static_cast<int>(frames_), 0);
    if (got == static_cast<int>(frames_)) return true;
    std::memset(out, 0, frames_ * std::size_t(channels_) * sizeof(float));
    return false;
}

} // namespace dc::lan
