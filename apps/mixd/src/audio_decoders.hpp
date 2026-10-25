#pragma once

// Thin wrapper over the vendored single-file decoders (third_party/audio_decoders), so only
// decoders_impl.cpp sees their headers.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dc::mixd {

struct RawAudio {
    std::vector<float> samples; // interleaved, the file's own rate and channel count
    int channels = 0;
    double sampleRate = 0;
};

// Detects WAV / FLAC / Ogg Vorbis by their signature and tries MP3 otherwise.
bool decodeAudioMemory(const std::uint8_t* data, std::size_t size, RawAudio& out, double maxSeconds, std::string* error);

} // namespace dc::mixd
