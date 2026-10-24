// WASAPI device backend: endpoint enumeration with change notifications, and one event-driven stream
// per device in shared mode (48 kHz float, Windows converts to the device's mix format) or exclusive
// mode (48 kHz in a PCM format the device accepts, period = /system/block_size).

#include "device.hpp"
#include "engine.hpp"

#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
// defines the PKEY_* constants in this translation unit
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace dc::mixd {

namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT / _PCM, spelled out to avoid ksmedia.h
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
// PKEY_AudioEngine_DeviceFormat: the device format, readable for unplugged endpoints too
constexpr PROPERTYKEY kDeviceFormatKey = {{0xf19f064d, 0x82c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

constexpr HRESULT kDeviceInvalidated = static_cast<HRESULT>(0x88890004L); // AUDCLNT_E_DEVICE_INVALIDATED
constexpr HRESULT kNotAligned = static_cast<HRESULT>(0x88890019L);        // AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED
constexpr HRESULT kDeviceInUse = static_cast<HRESULT>(0x8889000AL);       // AUDCLNT_E_DEVICE_IN_USE
constexpr REFERENCE_TIME kSharedInputBuffer = 1'000'000; // 100 ms
constexpr HRESULT kUnsupportedFormat = static_cast<HRESULT>(0x88890008L); // AUDCLNT_E_UNSUPPORTED_FORMAT
constexpr HRESULT kExclusiveNotAllowed = static_cast<HRESULT>(0x8889000EL); // AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED

template <typename T>
class Com {
public:
    Com() = default;
    Com(const Com& o) : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    Com& operator=(Com o) noexcept {
        std::swap(p_, o.p_);
        return *this;
    }
    ~Com() { reset(); }
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** put() {
        reset();
        return &p_;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }

private:
    T* p_ = nullptr;
};

std::string hrText(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    std::string s = buf;
    switch (hr) {
        case kDeviceInUse: s += " (他のアプリが排他モードで使用中)"; break;
        case kUnsupportedFormat: s += " (48kHz の形式に対応していない)"; break;
        case kExclusiveNotAllowed: s += " (デバイスの設定で排他モードが禁止されている)"; break;
        case kDeviceInvalidated: s += " (デバイスが外された)"; break;
        default: break;
    }
    return s;
}

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(std::max(n, 1)) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct ComInit {
    ComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInit() { CoUninitialize(); }
};

REFERENCE_TIME framesToHns(int frames) { return static_cast<REFERENCE_TIME>(frames * 10'000'000.0 / kSampleRate + 0.5); }
int hnsToFrames(REFERENCE_TIME hns) { return static_cast<int>(static_cast<double>(hns) * kSampleRate / 10'000'000.0 + 0.5); }

DWORD defaultMask(int channels) {
    switch (channels) {
        case 1: return SPEAKER_FRONT_CENTER;
        case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        default: return channels >= 32 ? 0xFFFFFFFFu : (1u << channels) - 1u;
    }
}

enum class SampleKind { Float32, Int32, Int24In32, Int24, Int16 };

struct Format {
    WAVEFORMATEXTENSIBLE wfx{};
    SampleKind kind = SampleKind::Float32;
    int channels = 2;
    int bytesPerSample = 4;
};

Format makeFormat(SampleKind kind, int channels, DWORD mask) {
    Format f;
    f.kind = kind;
    f.channels = channels;
    int container = 4, valid = 32;
    switch (kind) {
        case SampleKind::Float32: break;
        case SampleKind::Int32: break;
        case SampleKind::Int24In32: valid = 24; break;
        case SampleKind::Int24: container = 3; valid = 24; break;
        case SampleKind::Int16: container = 2; valid = 16; break;
    }
    f.bytesPerSample = container;
    auto& w = f.wfx;
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = static_cast<WORD>(channels);
    w.Format.nSamplesPerSec = static_cast<DWORD>(kSampleRate);
    w.Format.wBitsPerSample = static_cast<WORD>(container * 8);
    w.Format.nBlockAlign = static_cast<WORD>(container * channels);
    w.Format.nAvgBytesPerSec = w.Format.nSamplesPerSec * w.Format.nBlockAlign;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = static_cast<WORD>(valid);
    w.dwChannelMask = mask;
    w.SubFormat = kind == SampleKind::Float32 ? kSubtypeFloat : kSubtypePcm;
    return f;
}

// device bytes -> float, frames * channels samples
void toFloat(const Format& f, const BYTE* src, float* dst, std::size_t samples) {
    switch (f.kind) {
        case SampleKind::Float32: std::memcpy(dst, src, samples * 4); break;
        case SampleKind::Int32:
        case SampleKind::Int24In32: {
            const auto* s = reinterpret_cast<const std::int32_t*>(src);
            for (std::size_t i = 0; i < samples; ++i) dst[i] = static_cast<float>(s[i] * (1.0 / 2147483648.0));
            break;
        }
        case SampleKind::Int24:
            for (std::size_t i = 0; i < samples; ++i) {
                const BYTE* b = src + i * 3;
                const std::int32_t v = static_cast<std::int32_t>((std::uint32_t(b[0]) << 8) | (std::uint32_t(b[1]) << 16) |
                                                                 (std::uint32_t(b[2]) << 24));
                dst[i] = static_cast<float>(v * (1.0 / 2147483648.0));
            }
            break;
        case SampleKind::Int16: {
            const auto* s = reinterpret_cast<const std::int16_t*>(src);
            for (std::size_t i = 0; i < samples; ++i) dst[i] = s[i] * (1.0f / 32768.0f);
            break;
        }
    }
}

// float -> device bytes, clipped
void fromFloat(const Format& f, const float* src, BYTE* dst, std::size_t samples) {
    auto clip = [](float v) { return std::clamp(v, -1.0f, 1.0f); };
    switch (f.kind) {
        case SampleKind::Float32: std::memcpy(dst, src, samples * 4); break;
        case SampleKind::Int32:
        case SampleKind::Int24In32: {
            auto* d = reinterpret_cast<std::int32_t*>(dst);
            for (std::size_t i = 0; i < samples; ++i)
                d[i] = static_cast<std::int32_t>(std::clamp(static_cast<double>(src[i]) * 2147483648.0, -2147483648.0, 2147483647.0));
            break;
        }
        case SampleKind::Int24:
            for (std::size_t i = 0; i < samples; ++i) {
                const auto v = static_cast<std::int32_t>(clip(src[i]) * 8388607.0f);
                BYTE* b = dst + i * 3;
                b[0] = static_cast<BYTE>(v & 0xff);
                b[1] = static_cast<BYTE>((v >> 8) & 0xff);
                b[2] = static_cast<BYTE>((v >> 16) & 0xff);
            }
            break;
        case SampleKind::Int16: {
            auto* d = reinterpret_cast<std::int16_t*>(dst);
            for (std::size_t i = 0; i < samples; ++i) d[i] = static_cast<std::int16_t>(clip(src[i]) * 32767.0f);
            break;
        }
    }
}

class WasapiStream final : public Stream {
public:
    WasapiStream(bool input, InputCallback inCb, OutputCallback outCb, std::function<void()> onFailure)
        : input_(input), inCb_(std::move(inCb)), outCb_(std::move(outCb)), onFailure_(std::move(onFailure)) {}
    ~WasapiStream() override {
        stop();
        if (event_) CloseHandle(event_);
    }

    bool open(IMMDeviceEnumerator* en, const DeviceInfo& device, const StreamConfig& cfg, std::string* error) {
        exclusive_ = cfg.exclusive;
        const std::wstring id = widen(device.id);
        Com<IMMDevice> dev;
        HRESULT hr = en->GetDevice(id.c_str(), dev.put());
        if (FAILED(hr)) return fail(error, "GetDevice", hr);
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.putVoid());
        if (FAILED(hr)) return fail(error, "Activate", hr);

        WAVEFORMATEX* mix = nullptr;
        hr = client_->GetMixFormat(&mix);
        if (FAILED(hr)) return fail(error, "GetMixFormat", hr);
        int channels = mix->nChannels;
        const double mixRate = mix->nSamplesPerSec;
        DWORD mask = defaultMask(channels);
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
            mask = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
        CoTaskMemFree(mix);

        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        client_->GetDevicePeriod(&defPeriod, &minPeriod);
        if (defPeriod > 0) defaultPeriod_ = hnsToFrames(defPeriod);

        std::optional<Format> exclusiveFormat;
        if (exclusive_) {
            // the device format's channel count, then the mix format's
            for (auto kind : {SampleKind::Float32, SampleKind::Int32, SampleKind::Int24In32, SampleKind::Int24, SampleKind::Int16}) {
                auto f = makeFormat(kind, channels, mask);
                if (client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &f.wfx.Format, nullptr) == S_OK) {
                    exclusiveFormat = f;
                    break;
                }
            }
            // devices locked to another rate by their driver (Steinberg UR22 at 44.1 kHz, ...) cannot
            // run exclusive at 48 kHz: open them shared, where Windows converts the rate
            if (!exclusiveFormat) {
                exclusive_ = false;
                sharedFallback_ = true;
            }
        }

        if (!exclusive_) {
            format_ = makeFormat(SampleKind::Float32, channels, mask);
            const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            // inputs get a deeper buffer than one period: events still come every period, but a late
            // read no longer overflows it (G2: the UR22 reported data discontinuities with 10 ms)
            const REFERENCE_TIME buffer = input_ ? std::max<REFERENCE_TIME>(defPeriod, kSharedInputBuffer) : defPeriod;
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer, 0, &format_.wfx.Format, nullptr);
            if (FAILED(hr)) return fail(error, "Initialize (shared)", hr);
            period_ = hnsToFrames(defPeriod);
            devRate_ = mixRate > 0 ? mixRate : kSampleRate;
        } else {
            format_ = *exclusiveFormat;
            REFERENCE_TIME period = std::max(minPeriod, framesToHns(cfg.blockFrames));
            hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                     &format_.wfx.Format, nullptr);
            if (hr == kNotAligned) {
                // the "alignment dance": ask again with the size the device rounded to
                UINT32 frames = 0;
                client_->GetBufferSize(&frames);
                client_.reset();
                hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.putVoid());
                if (FAILED(hr)) return fail(error, "Activate", hr);
                period = static_cast<REFERENCE_TIME>(10'000'000.0 * frames / kSampleRate + 0.5);
                hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                         &format_.wfx.Format, nullptr);
            }
            if (FAILED(hr)) return fail(error, "Initialize (exclusive)", hr);
            period_ = hnsToFrames(period);
        }
        channels_ = format_.channels;

        UINT32 bufferFrames = 0;
        client_->GetBufferSize(&bufferFrames);
        bufferFrames_ = static_cast<int>(bufferFrames);
        if (exclusive_) period_ = bufferFrames_;
        REFERENCE_TIME streamLatency = 0;
        client_->GetStreamLatency(&streamLatency);
        latency_ = static_cast<double>(bufferFrames_) / kSampleRate + static_cast<double>(streamLatency) * 1e-7;

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        client_->SetEventHandle(event_);
        if (input_) {
            hr = client_->GetService(__uuidof(IAudioCaptureClient), capture_.putVoid());
            if (FAILED(hr)) return fail(error, "GetService(capture)", hr);
        } else {
            hr = client_->GetService(__uuidof(IAudioRenderClient), render_.putVoid());
            if (FAILED(hr)) return fail(error, "GetService(render)", hr);
            client_->GetService(__uuidof(IAudioClock), clock_.putVoid());
            if (clock_) clock_->GetFrequency(&clockFreq_);
        }
        scratch_.assign(static_cast<std::size_t>(std::max(bufferFrames_, 8192)) * static_cast<std::size_t>(channels_), 0.0f);
        return true;
    }

    bool start(std::string* error) override {
        if (!input_) {
            // preroll: start with a full buffer of silence so the first period cannot underflow
            BYTE* data = nullptr;
            const UINT32 frames = static_cast<UINT32>(bufferFrames_);
            if (SUCCEEDED(render_->GetBuffer(frames, &data))) render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
            written_ = frames;
        }
        const HRESULT hr = client_->Start();
        if (FAILED(hr)) return fail(error, "Start", hr);
        stop_ = false;
        thread_ = std::thread([this] { input_ ? captureLoop() : renderLoop(); });
        return true;
    }

    void stop() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (client_) client_->Stop();
    }

    int channels() const override { return channels_; }
    int periodFrames() const override { return period_; }
    double latencySeconds() const override { return latency_; }
    bool failed() const override { return failed_.load(); }
    std::uint64_t glitches() const override { return glitches_.load(); }
    bool sharedFallback() const override { return sharedFallback_; }
    int suggestedPeriod() const override { return suggestedPeriod_.load(); }
    std::string diagnostics() const override {
        // frames per second the device actually took or gave since start: should be about 48000
        std::string out;
        const double t0 = startTime_.load();
        if (t0 > 0) {
            const double secs = qpcSecondsNow() - t0;
            if (secs > 1) out += ", rate " + std::to_string(static_cast<long long>(static_cast<double>(frames_.load()) / secs)) + " Hz";
        }
        if (duplicates_.load()) out += ", duplicate frames dropped " + std::to_string(duplicates_.load());
        if (!input_ || glitches_.load() == 0) return out;
        // a late read shows as a long wait before the discontinuity; a device-side drop as a short one
        return out + ", last discontinuity: " + std::to_string(lastGapUs_.load() / 1000.0).substr(0, 6) +
               " ms since the read before, " + std::to_string(lastSkipped_.load()) + " frames skipped, longest wait " +
               std::to_string(maxGapUs_.load() / 1000.0).substr(0, 6) + " ms";
    }

private:
    bool fail(std::string* error, const char* what, HRESULT hr) {
        if (error) *error = std::string(what) + ": " + hrText(hr);
        return false;
    }

    // Exclusive mode: the device should wake us once per period. Measure the first second; a device
    // that wakes much less often cannot be fed with this period. True = give up (failed, suggestion).
    bool eventRateWrong(double now) {
        if (!exclusive_ || events_ > kRateCheckEvents) return false;
        if (++events_ == kRateSkipEvents) firstEvent_ = now;
        if (events_ < kRateCheckEvents) return false;
        const double perEvent = (now - firstEvent_) / (kRateCheckEvents - kRateSkipEvents) * kSampleRate;
        if (perEvent < 1.5 * period_) return false;
        // wake-ups jitter a little: about the device's default period means that period (G2: the
        // UR22 measured just over 480 and rounding up to 512 starved it)
        suggestedPeriod_ = std::abs(perEvent - defaultPeriod_) < 0.1 * defaultPeriod_
                               ? defaultPeriod_
                               : static_cast<int>(std::lround(perEvent / 32)) * 32;
        markFailed();
        return true;
    }

    // Exclusive capture: a device that overflows its buffer over and over in the first second (the
    // UR22 wakes every 10 ms but loses data with a 256-frame buffer) gets the device's own default
    // period when that is longer (G2: 512 made the UR22 wake every 20 ms and still lose data), else
    // twice the current one.
    bool captureOverflowing() {
        if (!exclusive_ || events_ > kRateCheckEvents || glitches_.load() < kOverflowLimit) return false;
        const int suggestion = defaultPeriod_ > period_ ? defaultPeriod_ : 2 * period_;
        suggestedPeriod_ = (suggestion + 31) / 32 * 32;
        markFailed();
        return true;
    }

    void markFailed() {
        failed_ = true;
        if (onFailure_) onFailure_();
    }

    void proAudio() {
        DWORD task = 0;
        avrt_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        if (avrt_) AvSetMmThreadPriority(avrt_, AVRT_PRIORITY_HIGH);
        else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    void renderLoop() {
        ComInit com;
        proAudio();
        int silentWaits = 0;
        double lastEvent = 0;
        while (!stop_) {
            if (WaitForSingleObject(event_, 200) != WAIT_OBJECT_0) {
                if (++silentWaits >= 10) { // 2 s without a period: the device stopped working
                    markFailed();
                    break;
                }
                continue;
            }
            silentWaits = 0;
            const double now = qpcSecondsNow();
            UINT32 padding = 0;
            int frames = bufferFrames_;
            if (!exclusive_) {
                const HRESULT hr = client_->GetCurrentPadding(&padding);
                if (FAILED(hr)) {
                    markFailed();
                    break;
                }
                if (padding == 0) glitches_.fetch_add(1); // the device ran out of audio
                frames = bufferFrames_ - static_cast<int>(padding);
                if (frames <= 0) continue;
            } else if (lastEvent > 0 && now - lastEvent > 1.8 * period_ / kSampleRate) {
                glitches_.fetch_add(1); // a period was missed
            }
            lastEvent = now;
            if (eventRateWrong(now)) break;

            // when the first new frame will be played (QPC seconds): the device position at a QPC
            // instant plus what is queued ahead of it
            double when = now + static_cast<double>(padding) / kSampleRate;
            if (clock_ && clockFreq_) {
                UINT64 pos = 0, qpc = 0;
                if (SUCCEEDED(clock_->GetPosition(&pos, &qpc)))
                    when = static_cast<double>(qpc) * 1e-7 +
                           (static_cast<double>(written_) - static_cast<double>(pos) / static_cast<double>(clockFreq_) * kSampleRate) /
                               kSampleRate;
            }

            BYTE* data = nullptr;
            HRESULT hr = render_->GetBuffer(static_cast<UINT32>(frames), &data);
            if (FAILED(hr)) {
                markFailed();
                break;
            }
            if (format_.kind == SampleKind::Float32) {
                outCb_(reinterpret_cast<float*>(data), frames, channels_, when);
            } else {
                outCb_(scratch_.data(), frames, channels_, when);
                fromFloat(format_, scratch_.data(), data, static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels_));
            }
            hr = render_->ReleaseBuffer(static_cast<UINT32>(frames), 0);
            if (FAILED(hr)) {
                markFailed();
                break;
            }
            written_ += static_cast<UINT64>(frames);
            frames_.fetch_add(static_cast<std::uint64_t>(frames));
            if (startTime_.load() == 0) startTime_ = now;
        }
        if (avrt_) AvRevertMmThreadCharacteristics(avrt_);
    }

    void captureLoop() {
        ComInit com;
        proAudio();
        int silentWaits = 0;
        bool first = true;
        double lastRead = 0;
        double expectedPos = 0; // 48 kHz frames
        while (!stop_) {
            if (WaitForSingleObject(event_, 200) != WAIT_OBJECT_0) {
                if (++silentWaits >= 10) {
                    markFailed();
                    break;
                }
                continue;
            }
            silentWaits = 0;
            const double now = qpcSecondsNow();
            const auto gapUs = lastRead > 0 ? static_cast<std::int64_t>((now - lastRead) * 1e6) : 0;
            lastRead = now;
            if (gapUs > maxGapUs_.load()) maxGapUs_ = gapUs;
            if (eventRateWrong(now)) break;
            UINT32 next = 0;
            HRESULT hr = S_OK;
            // device positions count frames at the device rate (44.1 kHz for a device converted in
            // shared mode): compare them in 48 kHz frames
            const double toStream = kSampleRate / devRate_;
            while (SUCCEEDED(hr = capture_->GetNextPacketSize(&next)) && next > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 devPos = 0, qpc = 0;
                hr = capture_->GetBuffer(&data, &frames, &flags, &devPos, &qpc);
                if (FAILED(hr)) break;
                if (hr == AUDCLNT_S_BUFFER_EMPTY || frames == 0) break;
                const double pos = static_cast<double>(devPos) * toStream;
                // a packet the device hands out again (G2: the Realtek mic in exclusive mode, about
                // 1.5x the real rate): drop what was delivered already, judged by its position
                UINT32 skip = 0;
                if (!first && devRate_ == kSampleRate && pos + 0.5 < expectedPos) {
                    skip = static_cast<UINT32>(std::min<double>(frames, expectedPos - pos + 0.5));
                    duplicates_.fetch_add(skip);
                }
                const bool discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0 && !first;
                if (discontinuity) {
                    lastGapUs_ = gapUs;
                    lastSkipped_ = static_cast<std::int64_t>(std::llround(pos - expectedPos));
                    glitches_.fetch_add(1);
                }
                expectedPos = std::max(expectedPos, pos + frames);
                first = false;
                const UINT32 fresh = frames - skip;
                const std::size_t samples = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels_);
                if (fresh > 0 && samples <= scratch_.size()) {
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill_n(scratch_.data(), samples, 0.0f);
                    else toFloat(format_, data, scratch_.data(), samples);
                    inCb_(scratch_.data() + static_cast<std::size_t>(skip) * static_cast<std::size_t>(channels_),
                          static_cast<int>(fresh), static_cast<double>(qpc) * 1e-7 + skip / kSampleRate, discontinuity);
                }
                frames_.fetch_add(fresh);
                if (startTime_.load() == 0) startTime_ = now;
                hr = capture_->ReleaseBuffer(frames);
                if (FAILED(hr)) break;
            }
            if (FAILED(hr)) {
                markFailed();
                break;
            }
            if (captureOverflowing()) break;
        }
        if (avrt_) AvRevertMmThreadCharacteristics(avrt_);
    }

    const bool input_;
    InputCallback inCb_;
    OutputCallback outCb_;
    std::function<void()> onFailure_;
    bool exclusive_ = false;
    bool sharedFallback_ = false;
    Com<IAudioClient> client_;
    Com<IAudioCaptureClient> capture_;
    Com<IAudioRenderClient> render_;
    Com<IAudioClock> clock_;
    UINT64 clockFreq_ = 0;
    UINT64 written_ = 0;
    HANDLE event_ = nullptr;
    HANDLE avrt_ = nullptr;
    Format format_;
    int channels_ = 0;
    int period_ = 480;
    int bufferFrames_ = 0;
    double latency_ = 0;
    std::vector<float> scratch_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> glitches_{0};
    std::atomic<std::int64_t> lastGapUs_{0}, maxGapUs_{0}, lastSkipped_{0};
    std::atomic<std::uint64_t> frames_{0}; // frames rendered or captured since the first period
    std::atomic<std::uint64_t> duplicates_{0}; // captured frames dropped as already delivered
    double devRate_ = kSampleRate;         // rate of the device positions
    static constexpr int kRateSkipEvents = 10, kRateCheckEvents = 110, kOverflowLimit = 10;
    int defaultPeriod_ = 480; // the device's default period, frames
    int events_ = 0;                       // stream thread only
    double firstEvent_ = 0;
    std::atomic<int> suggestedPeriod_{0};
    std::atomic<double> startTime_{0};
    std::thread thread_;
};

// Forwards endpoint changes (plugged, unplugged, added, removed, default changed) to a callback.
class NotificationClient final : public IMMNotificationClient {
public:
    explicit NotificationClient(std::function<void()> cb) : cb_(std::move(cb)) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return notify(); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return notify(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return notify(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole role, LPCWSTR) override {
        return role == eConsole ? notify() : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    HRESULT notify() {
        if (cb_) cb_();
        return S_OK;
    }
    std::atomic<ULONG> refs_{1};
    std::function<void()> cb_;
};

class WasapiBackend final : public DeviceBackend {
public:
    WasapiBackend() {
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                         enumerator_.putVoid());
        if (enumerator_) {
            notifier_ = new NotificationClient([this] { changed(); });
            enumerator_->RegisterEndpointNotificationCallback(notifier_);
        }
    }
    ~WasapiBackend() override {
        if (enumerator_ && notifier_) enumerator_->UnregisterEndpointNotificationCallback(notifier_);
        if (notifier_) notifier_->Release();
    }

    std::vector<DeviceInfo> enumerate() override {
        std::vector<DeviceInfo> out;
        if (!enumerator_) return out;
        std::string defaults[2];
        for (int i = 0; i < 2; ++i) {
            Com<IMMDevice> def;
            if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(i == 0 ? eRender : eCapture, eConsole, def.put()))) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(def->GetId(&id))) {
                    defaults[i] = narrow(id);
                    CoTaskMemFree(id);
                }
            }
        }
        Com<IMMDeviceCollection> col;
        if (FAILED(enumerator_->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, col.put())))
            return out;
        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            Com<IMMDevice> dev;
            if (FAILED(col->Item(i, dev.put()))) continue;
            DeviceInfo info;
            LPWSTR id = nullptr;
            if (FAILED(dev->GetId(&id))) continue;
            info.id = narrow(id);
            CoTaskMemFree(id);
            Com<IMMEndpoint> ep;
            EDataFlow flow = eRender;
            if (SUCCEEDED(dev->QueryInterface(__uuidof(IMMEndpoint), ep.putVoid()))) ep->GetDataFlow(&flow);
            info.input = flow == eCapture;
            DWORD state = 0;
            dev->GetState(&state);
            info.connected = state == DEVICE_STATE_ACTIVE;
            Com<IPropertyStore> props;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) info.name = narrow(v.pwszVal);
                PropVariantClear(&v);
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(kDeviceFormatKey, &v)) && v.vt == VT_BLOB && v.blob.cbSize >= sizeof(WAVEFORMATEX))
                    info.channels = reinterpret_cast<const WAVEFORMATEX*>(v.blob.pBlobData)->nChannels;
                PropVariantClear(&v);
            }
            if (info.connected) {
                // the mix format is what shared mode streams get
                Com<IAudioClient> client;
                if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid()))) {
                    WAVEFORMATEX* mix = nullptr;
                    if (SUCCEEDED(client->GetMixFormat(&mix))) {
                        info.channels = mix->nChannels;
                        CoTaskMemFree(mix);
                    }
                }
            }
            info.isDefault = info.id == defaults[info.input ? 1 : 0];
            info.virtualCable = info.name.find("VB-Audio") != std::string::npos || info.name.find("CABLE") != std::string::npos;
            out.push_back(std::move(info));
        }
        // stable order: defaults first, then by name
        std::stable_sort(out.begin(), out.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
            if (a.isDefault != b.isDefault) return a.isDefault;
            return a.name < b.name;
        });
        return out;
    }

    void setChangeCallback(std::function<void()> cb) override {
        std::lock_guard lock(mutex_);
        cb_ = std::move(cb);
    }

    std::unique_ptr<Stream> openInput(const DeviceInfo& device, const StreamConfig& cfg, InputCallback cb,
                                      std::string* error) override {
        auto s = std::make_unique<WasapiStream>(true, std::move(cb), nullptr, [this] { changed(); });
        if (!enumerator_ || !s->open(enumerator_.get(), device, cfg, error)) return nullptr;
        return s;
    }

    std::unique_ptr<Stream> openOutput(const DeviceInfo& device, const StreamConfig& cfg, OutputCallback cb,
                                       std::string* error) override {
        auto s = std::make_unique<WasapiStream>(false, nullptr, std::move(cb), [this] { changed(); });
        if (!enumerator_ || !s->open(enumerator_.get(), device, cfg, error)) return nullptr;
        return s;
    }

private:
    void changed() {
        std::lock_guard lock(mutex_);
        if (cb_) cb_();
    }

    ComInit com_;
    Com<IMMDeviceEnumerator> enumerator_;
    NotificationClient* notifier_ = nullptr;
    std::mutex mutex_;
    std::function<void()> cb_;
};

} // namespace

std::unique_ptr<DeviceBackend> makeWasapiBackend() { return std::make_unique<WasapiBackend>(); }

} // namespace dc::mixd
