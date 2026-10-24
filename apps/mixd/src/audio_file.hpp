#pragma once

// Audio files for the recorder and the sampler: 24-bit WAV / FLAC writers (recorder, written from its
// disk thread) and a decoder that turns WAV / FLAC / MP3 / Ogg Vorbis into 48 kHz stereo float
// (sampler pads, loaded on the loader thread). Paths are UTF-8.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace dc::mixd {

std::filesystem::path pathFromUtf8(const std::string& utf8);
std::string pathToUtf8(const std::filesystem::path& p);
std::FILE* openFile(const std::filesystem::path& p, const char* mode);

// Writes interleaved float frames as 48 kHz 24-bit integer PCM.
class AudioFileWriter {
public:
    virtual ~AudioFileWriter() = default;
    // false on a write error (disk full, ...); error() says why. The file stays valid up to the last
    // successful write once close() ran.
    virtual bool write(const float* interleaved, std::size_t frames) = 0;
    virtual bool writeSilence(std::size_t frames);
    virtual bool close() = 0;
    virtual std::uint64_t frames() const = 0;
    const std::string& error() const { return error_; }
    int channels() const { return channels_; }

protected:
    explicit AudioFileWriter(int channels) : channels_(channels) {}
    int channels_;
    std::string error_;
};

enum class AudioFileFormat { Wav, Flac };

// Creates the file (fails if it cannot be created). WAV switches to RF64 when the data passes 4 GiB
// and rewrites its header every few seconds, so a file cut off by a crash still opens.
std::unique_ptr<AudioFileWriter> createAudioFile(const std::filesystem::path& p, AudioFileFormat format, int channels,
                                                 std::string* error);

struct DecodedAudio {
    std::vector<float> samples; // interleaved stereo, 48 kHz
    std::size_t frames = 0;
};

// Reads a whole file. Mono becomes two equal channels, more than two channels keep the first two.
// Other rates are resampled to 48 kHz. Files longer than maxSeconds are refused.
bool decodeAudioFile(const std::filesystem::path& p, DecodedAudio& out, std::string* error, double maxSeconds = 600);

// Offline windowed-sinc resampler for interleaved frames (any rate ratio; lowers the cutoff when
// downsampling). Exposed for the tests.
std::vector<float> resampleInterleaved(const std::vector<float>& in, int channels, double inRate, double outRate);

} // namespace dc::mixd
