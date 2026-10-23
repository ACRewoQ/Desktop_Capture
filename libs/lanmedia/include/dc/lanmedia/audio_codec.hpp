#pragma once

#include "dc/lanmedia/packet.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

struct OpusEncoder;
struct OpusDecoder;

namespace dc::lan {

// Bytes of one packet's payload for a PCM codec.
std::size_t pcmBytes(AudioCodec codec, int channels, std::size_t frames);

// Frames per packet the codec accepts at 48 kHz. PCM: anything that fits a datagram. Opus: 2.5, 5,
// 10 or 20 ms (120, 240, 480, 960 frames).
bool validPacketFrames(AudioCodec codec, int channels, std::size_t frames);

// Encodes one packet of interleaved float frames. Real-time safe after construction (Opus allocates
// its state once).
class AudioEncoder {
public:
    struct Config {
        AudioCodec codec = AudioCodec::Pcm24;
        int channels = 2;
        std::size_t frames = 120;
        int opusBitrate = 128000; // bits per second
        bool opusFec = true;      // in-band forward error correction (the receiver rebuilds one lost packet)
        int opusExpectedLossPercent = 5;
    };

    explicit AudioEncoder(Config config);
    ~AudioEncoder();
    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    // in: config.frames interleaved frames. Returns bytes written to out, 0 on error.
    std::size_t encode(const float* in, std::uint8_t* out, std::size_t maxBytes);

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
    OpusEncoder* opus_ = nullptr;
};

// Decodes packets of one stream. For Opus the packets must be fed in order (lost ones through
// conceal() or decodeFec()), because the decoder carries state from packet to packet.
class AudioDecoder {
public:
    AudioDecoder(AudioCodec codec, int channels, std::size_t frames);
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // Decodes one packet into frames() interleaved frames. False if the payload is malformed.
    bool decode(std::span<const std::uint8_t> data, float* out);
    // Opus only: rebuilds the packet before `next` from next's FEC data. False if unavailable.
    bool decodeFec(std::span<const std::uint8_t> next, float* out);
    // Opus only: packet loss concealment for one missing packet.
    bool conceal(float* out);

    AudioCodec codec() const { return codec_; }
    int channels() const { return channels_; }
    std::size_t frames() const { return frames_; }

private:
    AudioCodec codec_;
    int channels_;
    std::size_t frames_;
    OpusDecoder* opus_ = nullptr;
};

} // namespace dc::lan
