// Implementation unit for the vendored decoders. Built without the project's warning flags.

#include "audio_decoders.hpp"

#include <cstring>
#include <string>

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
#undef L
#undef C
#undef R

namespace dc::mixd {

namespace {

bool tooLong(std::uint64_t frames, double rate, double maxSeconds, std::string* error) {
    if (rate <= 0 || static_cast<double>(frames) / rate <= maxSeconds) return false;
    if (error) *error = "longer than " + std::to_string(static_cast<int>(maxSeconds / 60)) + " minutes";
    return true;
}

bool finish(RawAudio& out, int channels, double rate, std::uint64_t framesRead, std::string* error) {
    if (channels <= 0 || rate <= 0 || framesRead == 0) {
        if (error) *error = "no audio in the file";
        return false;
    }
    out.channels = channels;
    out.sampleRate = rate;
    out.samples.resize(static_cast<std::size_t>(framesRead) * static_cast<std::size_t>(channels));
    return true;
}

bool decodeWav(const std::uint8_t* data, std::size_t size, RawAudio& out, double maxSeconds, std::string* error) {
    drwav wav;
    if (!drwav_init_memory(&wav, data, size, nullptr)) {
        if (error) *error = "unsupported WAV file";
        return false;
    }
    bool ok = false;
    if (!tooLong(wav.totalPCMFrameCount, wav.sampleRate, maxSeconds, error)) {
        out.samples.resize(static_cast<std::size_t>(wav.totalPCMFrameCount) * wav.channels);
        const auto n = drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, out.samples.data());
        ok = finish(out, wav.channels, wav.sampleRate, n, error);
    }
    drwav_uninit(&wav);
    return ok;
}

bool decodeFlac(const std::uint8_t* data, std::size_t size, RawAudio& out, double maxSeconds, std::string* error) {
    drflac* flac = drflac_open_memory(data, size, nullptr);
    if (!flac) {
        if (error) *error = "unsupported FLAC file";
        return false;
    }
    bool ok = false;
    if (!tooLong(flac->totalPCMFrameCount, flac->sampleRate, maxSeconds, error)) {
        out.samples.resize(static_cast<std::size_t>(flac->totalPCMFrameCount) * flac->channels);
        const auto n = drflac_read_pcm_frames_f32(flac, flac->totalPCMFrameCount, out.samples.data());
        ok = finish(out, flac->channels, flac->sampleRate, n, error);
    }
    drflac_close(flac);
    return ok;
}

bool decodeMp3(const std::uint8_t* data, std::size_t size, RawAudio& out, double maxSeconds, std::string* error) {
    drmp3 mp3;
    if (!drmp3_init_memory(&mp3, data, size, nullptr)) {
        if (error) *error = "unsupported file format (WAV, FLAC, MP3 or Ogg Vorbis)";
        return false;
    }
    bool ok = false;
    const auto total = drmp3_get_pcm_frame_count(&mp3); // walks the file, leaves the decoder at the start
    if (total == 0) {
        if (error) *error = "unsupported file format (WAV, FLAC, MP3 or Ogg Vorbis)";
    } else if (!tooLong(total, mp3.sampleRate, maxSeconds, error)) {
        out.samples.resize(static_cast<std::size_t>(total) * mp3.channels);
        const auto n = drmp3_read_pcm_frames_f32(&mp3, total, out.samples.data());
        ok = finish(out, static_cast<int>(mp3.channels), mp3.sampleRate, n, error);
    }
    drmp3_uninit(&mp3);
    return ok;
}

bool decodeVorbis(const std::uint8_t* data, std::size_t size, RawAudio& out, double maxSeconds, std::string* error) {
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(data, static_cast<int>(size), &err, nullptr);
    if (!v) {
        if (error) *error = "unsupported Ogg file (only Vorbis)";
        return false;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    const unsigned total = stb_vorbis_stream_length_in_samples(v);
    bool ok = false;
    if (!tooLong(total, info.sample_rate, maxSeconds, error)) {
        out.samples.resize(static_cast<std::size_t>(total) * static_cast<std::size_t>(info.channels));
        std::size_t done = 0;
        while (done < total) {
            const int n = stb_vorbis_get_samples_float_interleaved(
                v, info.channels, out.samples.data() + done * static_cast<std::size_t>(info.channels),
                static_cast<int>((total - done) * static_cast<std::size_t>(info.channels)));
            if (n <= 0) break;
            done += static_cast<std::size_t>(n);
        }
        ok = finish(out, info.channels, info.sample_rate, done, error);
    }
    stb_vorbis_close(v);
    return ok;
}

} // namespace

bool decodeAudioMemory(const std::uint8_t* data, std::size_t size, RawAudio& out, double maxSeconds, std::string* error) {
    auto sig = [&](const char* s) { return size >= 4 && std::memcmp(data, s, 4) == 0; };
    if (sig("RIFF") || sig("RF64") || sig("riff")) return decodeWav(data, size, out, maxSeconds, error);
    if (sig("fLaC")) return decodeFlac(data, size, out, maxSeconds, error);
    if (sig("OggS")) return decodeVorbis(data, size, out, maxSeconds, error);
    return decodeMp3(data, size, out, maxSeconds, error);
}

} // namespace dc::mixd
