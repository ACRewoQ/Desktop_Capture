#include "audio_file.hpp"

#include "audio_decoders.hpp"

#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dc::mixd {

std::filesystem::path pathFromUtf8(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string pathToUtf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

std::FILE* openFile(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
    std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(p.c_str(), wmode.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

namespace {

bool seekTo(std::FILE* f, std::uint64_t pos) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<long long>(pos), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(pos), SEEK_SET) == 0;
#endif
}

std::uint64_t tellPos(std::FILE* f) {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_ftelli64(f));
#else
    return static_cast<std::uint64_t>(ftello(f));
#endif
}

std::int32_t toInt24(float v) {
    const double s = std::clamp(static_cast<double>(v), -1.0, 1.0) * 8388607.0;
    return static_cast<std::int32_t>(std::lround(s));
}

void put16(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// RIFF / RF64 WAVE, 48 kHz 24-bit PCM. Layout: RIFF header, a 28-byte JUNK chunk that becomes the
// ds64 chunk when the file outgrows 4 GiB (EBU Tech 3306), fmt, data.
class WavWriter final : public AudioFileWriter {
public:
    static constexpr std::size_t kHeaderBytes = 12 + 36 + 24 + 8;
    static constexpr std::uint64_t kRewriteEvery = 48000 * 5; // frames between header rewrites

    WavWriter(std::FILE* f, int channels) : AudioFileWriter(channels), file_(f) {
        buffer_.resize(static_cast<std::size_t>(4096 * channels * 3));
        writeHeader();
    }
    ~WavWriter() override { close(); }

    bool write(const float* x, std::size_t frames) override {
        if (!file_ || failed_) return false;
        const std::size_t bpf = static_cast<std::size_t>(channels_) * 3;
        while (frames > 0) {
            const std::size_t n = std::min<std::size_t>(frames, 4096);
            std::uint8_t* p = buffer_.data();
            for (std::size_t i = 0; i < n * static_cast<std::size_t>(channels_); ++i) {
                const std::int32_t s = toInt24(x[i]);
                p[0] = static_cast<std::uint8_t>(s);
                p[1] = static_cast<std::uint8_t>(s >> 8);
                p[2] = static_cast<std::uint8_t>(s >> 16);
                p += 3;
            }
            if (std::fwrite(buffer_.data(), 1, n * bpf, file_) != n * bpf) return fail("write failed (disk full?)");
            frames_ += n;
            x += n * static_cast<std::size_t>(channels_);
            frames -= n;
        }
        if (frames_ - lastHeader_ >= kRewriteEvery) {
            lastHeader_ = frames_;
            if (!rewriteHeader()) return fail("write failed (disk full?)");
        }
        return true;
    }

    bool close() override {
        if (!file_) return !failed_;
        const std::uint64_t data = frames_ * static_cast<std::uint64_t>(channels_) * 3;
        bool ok = !failed_;
        if (data & 1) ok = std::fputc(0, file_) != EOF && ok; // chunks are padded to an even size
        ok = rewriteHeader() && ok;
        ok = std::fclose(file_) == 0 && ok;
        file_ = nullptr;
        if (!ok && error_.empty()) error_ = "closing the file failed";
        return ok;
    }

    std::uint64_t frames() const override { return frames_; }

private:
    bool fail(const char* what) {
        failed_ = true;
        if (error_.empty()) error_ = what;
        return false;
    }

    void fillHeader(std::array<std::uint8_t, kHeaderBytes>& h) const {
        const std::uint64_t data = frames_ * static_cast<std::uint64_t>(channels_) * 3;
        const std::uint64_t riff = kHeaderBytes - 8 + data + (data & 1);
        const bool rf64 = riff > 0xFFFFFFFFull;
        h.fill(0);
        std::memcpy(&h[0], rf64 ? "RF64" : "RIFF", 4);
        put32(&h[4], rf64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(riff));
        std::memcpy(&h[8], "WAVE", 4);
        std::memcpy(&h[12], rf64 ? "ds64" : "JUNK", 4);
        put32(&h[16], 28);
        if (rf64) {
            put64(&h[20], riff);
            put64(&h[28], data);
            put64(&h[36], frames_);
            put32(&h[44], 0); // no table
        }
        std::memcpy(&h[48], "fmt ", 4);
        put32(&h[52], 16);
        put16(&h[56], 1); // PCM
        put16(&h[58], static_cast<std::uint32_t>(channels_));
        put32(&h[60], 48000);
        put32(&h[64], 48000u * static_cast<std::uint32_t>(channels_) * 3u);
        put16(&h[68], static_cast<std::uint32_t>(channels_) * 3u);
        put16(&h[70], 24);
        std::memcpy(&h[72], "data", 4);
        put32(&h[76], rf64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(data));
    }

    void writeHeader() {
        std::array<std::uint8_t, kHeaderBytes> h{};
        fillHeader(h);
        if (std::fwrite(h.data(), 1, h.size(), file_) != h.size()) fail("write failed");
    }

    bool rewriteHeader() {
        std::array<std::uint8_t, kHeaderBytes> h{};
        fillHeader(h);
        const std::uint64_t end = tellPos(file_);
        if (!seekTo(file_, 0)) return false;
        const bool ok = std::fwrite(h.data(), 1, h.size(), file_) == h.size();
        return seekTo(file_, end) && ok && std::fflush(file_) == 0;
    }

    std::FILE* file_;
    std::vector<std::uint8_t> buffer_;
    std::uint64_t frames_ = 0;
    std::uint64_t lastHeader_ = 0;
    bool failed_ = false;
};

// FLAC through libFLAC's stream encoder; our own callbacks write to the FILE* so UTF-8 paths work.
class FlacWriter final : public AudioFileWriter {
public:
    FlacWriter(std::FILE* f, int channels) : AudioFileWriter(channels), file_(f), encoder_(FLAC__stream_encoder_new()) {
        buffer_.resize(static_cast<std::size_t>(4096 * channels));
        if (!encoder_) {
            error_ = "cannot create the FLAC encoder";
            return;
        }
        FLAC__stream_encoder_set_channels(encoder_, static_cast<unsigned>(channels));
        FLAC__stream_encoder_set_bits_per_sample(encoder_, 24);
        FLAC__stream_encoder_set_sample_rate(encoder_, 48000);
        FLAC__stream_encoder_set_compression_level(encoder_, 5);
        const auto st = FLAC__stream_encoder_init_stream(encoder_, &FlacWriter::writeCb, &FlacWriter::seekCb,
                                                         &FlacWriter::tellCb, nullptr, this);
        if (st != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            error_ = std::string("FLAC encoder: ") + FLAC__StreamEncoderInitStatusString[st];
            FLAC__stream_encoder_delete(encoder_);
            encoder_ = nullptr;
        }
    }
    ~FlacWriter() override { close(); }

    bool ok() const { return encoder_ != nullptr; }

    bool write(const float* x, std::size_t frames) override {
        if (!encoder_ || failed_) return false;
        while (frames > 0) {
            const std::size_t n = std::min<std::size_t>(frames, 4096);
            for (std::size_t i = 0; i < n * static_cast<std::size_t>(channels_); ++i) buffer_[i] = toInt24(x[i]);
            if (!FLAC__stream_encoder_process_interleaved(encoder_, buffer_.data(), static_cast<unsigned>(n))) {
                failed_ = true;
                if (error_.empty()) error_ = "write failed (disk full?)";
                return false;
            }
            frames_ += n;
            x += n * static_cast<std::size_t>(channels_);
            frames -= n;
        }
        return true;
    }

    bool close() override {
        bool ok = !failed_;
        if (encoder_) {
            ok = FLAC__stream_encoder_finish(encoder_) && ok; // writes the last frame and the final STREAMINFO
            FLAC__stream_encoder_delete(encoder_);
            encoder_ = nullptr;
        }
        if (file_) {
            ok = std::fclose(file_) == 0 && ok;
            file_ = nullptr;
        }
        if (!ok && error_.empty()) error_ = "closing the file failed";
        return ok;
    }

    std::uint64_t frames() const override { return frames_; }

private:
    static FLAC__StreamEncoderWriteStatus writeCb(const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes,
                                                  unsigned, unsigned, void* self) {
        auto* w = static_cast<FlacWriter*>(self);
        if (std::fwrite(buffer, 1, bytes, w->file_) != bytes) return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    }
    static FLAC__StreamEncoderSeekStatus seekCb(const FLAC__StreamEncoder*, FLAC__uint64 offset, void* self) {
        auto* w = static_cast<FlacWriter*>(self);
        return seekTo(w->file_, offset) ? FLAC__STREAM_ENCODER_SEEK_STATUS_OK : FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    }
    static FLAC__StreamEncoderTellStatus tellCb(const FLAC__StreamEncoder*, FLAC__uint64* offset, void* self) {
        *offset = tellPos(static_cast<FlacWriter*>(self)->file_);
        return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
    }

    std::FILE* file_;
    FLAC__StreamEncoder* encoder_;
    std::vector<FLAC__int32> buffer_;
    std::uint64_t frames_ = 0;
    bool failed_ = false;
};

} // namespace

bool AudioFileWriter::writeSilence(std::size_t frames) {
    static const std::array<float, 2048> zeros{};
    const std::size_t chunk = zeros.size() / static_cast<std::size_t>(channels_);
    while (frames > 0) {
        const std::size_t n = std::min(frames, chunk);
        if (!write(zeros.data(), n)) return false;
        frames -= n;
    }
    return true;
}

std::unique_ptr<AudioFileWriter> createAudioFile(const std::filesystem::path& p, AudioFileFormat format, int channels,
                                                 std::string* error) {
    std::FILE* f = openFile(p, "wb");
    if (!f) {
        if (error) *error = "cannot create " + pathToUtf8(p);
        return nullptr;
    }
    if (format == AudioFileFormat::Wav) return std::make_unique<WavWriter>(f, channels);
    auto w = std::make_unique<FlacWriter>(f, channels);
    if (!w->ok()) {
        if (error) *error = w->error();
        return nullptr;
    }
    return w;
}

// --- decoding ---

namespace {

bool readWholeFile(const std::filesystem::path& p, std::vector<std::uint8_t>& out, std::string* error) {
    std::FILE* f = openFile(p, "rb");
    if (!f) {
        if (error) *error = "cannot open the file";
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const std::uint64_t size = tellPos(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > (1ull << 30)) {
        std::fclose(f);
        if (error) *error = "file larger than 1 GB";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok && error) *error = "cannot read the file";
    return ok;
}

double besselI0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 40; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

} // namespace

std::vector<float> resampleInterleaved(const std::vector<float>& in, int channels, double inRate, double outRate) {
    const std::size_t inFrames = in.size() / static_cast<std::size_t>(channels);
    if (inRate == outRate || inFrames == 0) return in;

    constexpr int kHalfZeros = 24;   // zero crossings of the sinc on each side
    constexpr int kTableRes = 512;   // kernel samples per input sample (at cutoff 1)
    constexpr double kBeta = 9.0;
    const double cutoff = std::min(1.0, outRate / inRate) * 0.95; // relative to the input Nyquist
    const double halfWidth = kHalfZeros / cutoff;                 // in input samples

    // kernel h(u) for u in [0, halfWidth], sampled every 1 / kTableRes input samples
    const auto tableSize = static_cast<std::size_t>(std::ceil(halfWidth * kTableRes)) + 2;
    std::vector<float> table(tableSize);
    const double i0b = besselI0(kBeta);
    for (std::size_t i = 0; i < tableSize; ++i) {
        const double u = static_cast<double>(i) / kTableRes;
        if (u >= halfWidth) {
            table[i] = 0;
            continue;
        }
        const double x = u * cutoff;
        const double sinc = x == 0 ? 1.0 : std::sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x);
        const double r = u / halfWidth;
        table[i] = static_cast<float>(cutoff * sinc * besselI0(kBeta * std::sqrt(1 - r * r)) / i0b);
    }
    auto kernel = [&](double u) {
        u = std::fabs(u) * kTableRes;
        const auto i = static_cast<std::size_t>(u);
        if (i + 1 >= tableSize) return 0.0f;
        const float f = static_cast<float>(u - static_cast<double>(i));
        return table[i] + (table[i + 1] - table[i]) * f;
    };

    const double step = inRate / outRate;
    const auto outFrames = static_cast<std::size_t>(std::floor(static_cast<double>(inFrames) / step));
    std::vector<float> out(outFrames * static_cast<std::size_t>(channels), 0.0f);
    const int reach = static_cast<int>(std::ceil(halfWidth));
    std::vector<double> acc(static_cast<std::size_t>(channels));
    for (std::size_t n = 0; n < outFrames; ++n) {
        const double t = static_cast<double>(n) * step;
        const auto center = static_cast<std::ptrdiff_t>(std::floor(t));
        std::fill(acc.begin(), acc.end(), 0.0);
        for (std::ptrdiff_t k = center - reach + 1; k <= center + reach; ++k) {
            if (k < 0 || k >= static_cast<std::ptrdiff_t>(inFrames)) continue;
            const float w = kernel(static_cast<double>(k) - t);
            if (w == 0) continue;
            const float* src = &in[static_cast<std::size_t>(k) * static_cast<std::size_t>(channels)];
            for (int c = 0; c < channels; ++c) acc[static_cast<std::size_t>(c)] += src[c] * w;
        }
        for (int c = 0; c < channels; ++c)
            out[n * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] =
                static_cast<float>(acc[static_cast<std::size_t>(c)]);
    }
    return out;
}

bool decodeAudioFile(const std::filesystem::path& p, DecodedAudio& out, std::string* error, double maxSeconds) {
    std::vector<std::uint8_t> data;
    if (!readWholeFile(p, data, error)) return false;

    RawAudio raw;
    std::string err;
    if (!decodeAudioMemory(data.data(), data.size(), raw, maxSeconds, &err)) {
        if (error) *error = err;
        return false;
    }
    data.clear();
    data.shrink_to_fit();

    // to stereo
    std::vector<float> stereo;
    const std::size_t frames = raw.channels > 0 ? raw.samples.size() / static_cast<std::size_t>(raw.channels) : 0;
    if (raw.channels == 2) {
        stereo = std::move(raw.samples);
    } else {
        stereo.resize(frames * 2);
        for (std::size_t i = 0; i < frames; ++i) {
            const float* s = &raw.samples[i * static_cast<std::size_t>(raw.channels)];
            stereo[2 * i] = s[0];
            stereo[2 * i + 1] = raw.channels == 1 ? s[0] : s[1];
        }
        raw.samples.clear();
        raw.samples.shrink_to_fit();
    }

    out.samples = resampleInterleaved(stereo, 2, raw.sampleRate, 48000.0);
    out.frames = out.samples.size() / 2;
    return true;
}

} // namespace dc::mixd
