// WASAPI device backend: endpoint enumeration with change notifications, and one event-driven stream
// per device in shared mode (float at the device's mix rate) or exclusive mode (a PCM format the
// device accepts, 48 kHz when it can, period = /system/block_size). Devices that do not run at 48 kHz
// are converted inside the stream (clocksync::RateConverter), not by Windows. Also: process loopback
// captures of applications, and the ASIO drivers (asio_backend.cpp) listed next to the endpoints.

#include "asio_backend.hpp"
#include "device.hpp"
#include "engine.hpp"

#include "dc/clocksync/rate_converter.hpp"

#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include "process_loopback_compat.hpp"
// defines the PKEY_* constants in this translation unit
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
    ComInit() : ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComInit() {
        if (ok) CoUninitialize();
    }
    bool ok;
};

// 48 kHz frames <-> 100 ns units
REFERENCE_TIME framesToHns(int frames) { return static_cast<REFERENCE_TIME>(frames * 10'000'000.0 / kSampleRate + 0.5); }
int hnsToFrames(REFERENCE_TIME hns) { return static_cast<int>(static_cast<double>(hns) * kSampleRate / 10'000'000.0 + 0.5); }

// The rate of the device format (what the hardware runs at), 0 if unknown.
int deviceFormatRate(IMMDevice* dev) {
    Com<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, props.put()))) return 0;
    PROPVARIANT v;
    PropVariantInit(&v);
    int rate = 0;
    if (SUCCEEDED(props->GetValue(kDeviceFormatKey, &v)) && v.vt == VT_BLOB && v.blob.cbSize >= sizeof(WAVEFORMATEX))
        rate = static_cast<int>(reinterpret_cast<const WAVEFORMATEX*>(v.blob.pBlobData)->nSamplesPerSec);
    PropVariantClear(&v);
    return rate;
}

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

Format makeFormat(SampleKind kind, int channels, DWORD mask, int rate = static_cast<int>(kSampleRate)) {
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
    w.Format.nSamplesPerSec = static_cast<DWORD>(rate);
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

// ActivateAudioInterfaceAsync wants an agile completion handler: a free-threaded object that signals.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivationHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ActivationHandler() { CloseHandle(done_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject))
            *out = static_cast<IAgileObject*>(this);
        else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done_);
        return S_OK;
    }
    HANDLE done() const { return done_; }

private:
    std::atomic<ULONG> refs_{1};
    HANDLE done_;
};

// An IAudioClient capturing what the process tree of pid plays (V1: Windows 10 22H2 and 11).
HRESULT activateProcessLoopback(DWORD pid, IAudioClient** out) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof params;
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    auto* handler = new ActivationHandler();
    Com<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler, op.put());
    if (FAILED(hr)) {
        handler->Release();
        return hr;
    }
    const DWORD wait = WaitForSingleObject(handler->done(), 5000);
    handler->Release();
    if (wait != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    HRESULT activated = E_FAIL;
    Com<IUnknown> unk;
    hr = op->GetActivateResult(&activated, unk.put());
    if (FAILED(hr)) return hr;
    if (FAILED(activated)) return activated;
    return unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(out));
}

class WasapiStream final : public Stream {
public:
    WasapiStream(bool input, InputCallback inCb, OutputCallback outCb, std::function<void()> onFailure)
        : input_(input), inCb_(std::move(inCb)), outCb_(std::move(outCb)), onFailure_(std::move(onFailure)) {}
    ~WasapiStream() override {
        stop();
        if (event_) CloseHandle(event_);
        if (process_) CloseHandle(process_);
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
        const int mixRate = static_cast<int>(mix->nSamplesPerSec);
        DWORD mask = defaultMask(channels);
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
            mask = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
        CoTaskMemFree(mix);

        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        client_->GetDevicePeriod(&defPeriod, &minPeriod);
        if (defPeriod > 0) defaultPeriod_ = hnsToFrames(defPeriod);

        std::optional<Format> exclusiveFormat;
        if (exclusive_) {
            // 48 kHz first; a device its driver locks to another rate (the Steinberg UR22 at 44.1 kHz)
            // runs at that rate and the stream converts
            std::vector<int> rates{static_cast<int>(kSampleRate)};
            for (int r : {deviceFormatRate(dev.get()), mixRate, 44100, 96000, 88200})
                if (r > 0 && std::find(rates.begin(), rates.end(), r) == rates.end()) rates.push_back(r);
            for (int rate : rates) {
                for (auto kind : {SampleKind::Float32, SampleKind::Int32, SampleKind::Int24In32, SampleKind::Int24, SampleKind::Int16}) {
                    auto f = makeFormat(kind, channels, mask, rate);
                    if (client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &f.wfx.Format, nullptr) == S_OK) {
                        exclusiveFormat = f;
                        break;
                    }
                }
                if (exclusiveFormat) break;
            }
            // no exclusive PCM format at all: shared mode
            if (!exclusiveFormat) {
                exclusive_ = false;
                sharedFallback_ = true;
            }
        }

        if (!exclusive_) {
            // the mix format's own rate, so Windows does not resample (G2: its converter dropped data
            // every few minutes on the UR22 at 44.1 kHz)
            format_ = makeFormat(SampleKind::Float32, channels, mask, mixRate > 0 ? mixRate : static_cast<int>(kSampleRate));
            const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
            // inputs get a deeper buffer than one period: events still come every period, but a late
            // read no longer overflows it (G2: the UR22 reported data discontinuities with 10 ms)
            const REFERENCE_TIME buffer = input_ ? std::max<REFERENCE_TIME>(defPeriod, kSharedInputBuffer) : defPeriod;
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer, 0, &format_.wfx.Format, nullptr);
            if (FAILED(hr) && format_.wfx.Format.nSamplesPerSec != kSampleRate) {
                // as in P2: 48 kHz, converted by Windows
                client_.reset();
                hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.putVoid());
                if (FAILED(hr)) return fail(error, "Activate", hr);
                format_ = makeFormat(SampleKind::Float32, channels, mask);
                hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, buffer, 0,
                                         &format_.wfx.Format, nullptr);
                // device positions still count frames at the device rate
                posScale_ = mixRate > 0 ? kSampleRate / mixRate : 1.0;
            }
            if (FAILED(hr)) return fail(error, "Initialize (shared)", hr);
            period_ = hnsToFrames(defPeriod);
        } else {
            format_ = *exclusiveFormat;
            const double rate = format_.wfx.Format.nSamplesPerSec;
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
                period = static_cast<REFERENCE_TIME>(10'000'000.0 * frames / rate + 0.5);
                hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                         &format_.wfx.Format, nullptr);
            }
            if (FAILED(hr)) return fail(error, "Initialize (exclusive)", hr);
            period_ = hnsToFrames(period);
        }
        channels_ = format_.channels;
        devRate_ = format_.wfx.Format.nSamplesPerSec;

        UINT32 bufferFrames = 0;
        client_->GetBufferSize(&bufferFrames);
        bufferFrames_ = static_cast<int>(bufferFrames);
        if (exclusive_) period_ = static_cast<int>(std::lround(bufferFrames_ * kSampleRate / devRate_));
        REFERENCE_TIME streamLatency = 0;
        client_->GetStreamLatency(&streamLatency);
        latency_ = static_cast<double>(bufferFrames_) / devRate_ + static_cast<double>(streamLatency) * 1e-7;
        if (devRate_ != kSampleRate) {
            const auto block = static_cast<std::size_t>(std::max(bufferFrames_, 1024));
            converter_ = input_ ? std::make_unique<clocksync::RateConverter>(channels_, devRate_, kSampleRate, block)
                                : std::make_unique<clocksync::RateConverter>(channels_, kSampleRate, devRate_, block);
        }
        return finishOpen(error);
    }

    // Process loopback: what the process tree of pid plays, 48 kHz stereo (Windows mixes and converts).
    bool openProcess(const ProcessInfo& p, std::string* error) {
        process_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
        if (!process_) return fail(error, "OpenProcess", HRESULT_FROM_WIN32(GetLastError()));
        HRESULT hr = activateProcessLoopback(p.pid, client_.put());
        if (FAILED(hr)) return fail(error, "ActivateAudioInterfaceAsync (process loopback)", hr);
        // process loopback clients have no mix format or device period: the format is ours
        const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        format_ = makeFormat(SampleKind::Float32, 2, defaultMask(2));
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kSharedInputBuffer, 0, &format_.wfx.Format, nullptr);
        if (hr == kUnsupportedFormat) {
            format_ = makeFormat(SampleKind::Int16, 2, defaultMask(2));
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kSharedInputBuffer, 0, &format_.wfx.Format, nullptr);
        }
        if (FAILED(hr)) return fail(error, "Initialize (process loopback)", hr);
        channels_ = 2;
        devRate_ = kSampleRate;
        period_ = 480; // V1: a packet every 10 ms, silent ones too
        UINT32 bufferFrames = 0;
        client_->GetBufferSize(&bufferFrames);
        bufferFrames_ = static_cast<int>(bufferFrames);
        latency_ = static_cast<double>(bufferFrames_) / kSampleRate;
        return finishOpen(error);
    }

    bool finishOpen(std::string* error) {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        client_->SetEventHandle(event_);
        HRESULT hr = S_OK;
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
    int deviceRate() const override { return static_cast<int>(devRate_); }
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
                    when = static_cast<double>(qpc) * 1e-7 + static_cast<double>(written_) / devRate_ -
                           static_cast<double>(pos) / static_cast<double>(clockFreq_);
            }

            BYTE* data = nullptr;
            HRESULT hr = render_->GetBuffer(static_cast<UINT32>(frames), &data);
            if (FAILED(hr)) {
                markFailed();
                break;
            }
            float* dst = format_.kind == SampleKind::Float32 ? reinterpret_cast<float*>(data) : scratch_.data();
            if (converter_) {
                // the engine renders 48 kHz frames as the converter asks for them
                double supplied = 0;
                converter_->pull(dst, static_cast<std::size_t>(frames), [&](float* in, std::size_t n) {
                    outCb_(in, static_cast<int>(n), channels_, when + supplied / kSampleRate);
                    supplied += static_cast<double>(n);
                });
            } else {
                outCb_(dst, frames, channels_, when);
            }
            if (format_.kind != SampleKind::Float32)
                fromFloat(format_, scratch_.data(), data, static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels_));
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
            if (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
                // the application exited: the capture would only deliver silence from now on
                markFailed();
                break;
            }
            const double now = qpcSecondsNow();
            const auto gapUs = lastRead > 0 ? static_cast<std::int64_t>((now - lastRead) * 1e6) : 0;
            lastRead = now;
            if (gapUs > maxGapUs_.load()) maxGapUs_ = gapUs;
            if (eventRateWrong(now)) break;
            UINT32 next = 0;
            HRESULT hr = S_OK;
            // device positions count frames at the device rate; the data does too, except when Windows
            // converts to 48 kHz (posScale_)
            const double toStream = posScale_;
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
                // 1.5x the real rate): drop what was delivered already, judged by its position. Not for
                // process loopback: its packets carry no usable device position (A2 test: every packet
                // was dropped)
                UINT32 skip = 0;
                if (!first && !process_ && posScale_ == 1.0 && pos + 0.5 < expectedPos) {
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
                    deliver(scratch_.data() + static_cast<std::size_t>(skip) * static_cast<std::size_t>(channels_), fresh,
                            static_cast<double>(qpc) * 1e-7 + skip / devRate_, discontinuity);
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

    // captured frames at the device rate -> the callback at 48 kHz
    void deliver(const float* frames, UINT32 count, double time, bool discontinuity) {
        pendingDiscontinuity_ = pendingDiscontinuity_ || discontinuity;
        if (!converter_) {
            inCb_(frames, static_cast<int>(count), time, pendingDiscontinuity_);
            pendingDiscontinuity_ = false;
            return;
        }
        converter_->push(frames, count, time, [&](const float* out, std::size_t n, double t) {
            inCb_(out, static_cast<int>(n), t, pendingDiscontinuity_);
            pendingDiscontinuity_ = false;
        });
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
    double devRate_ = kSampleRate;  // rate of the stream's data (the device's own unless Windows converts)
    double posScale_ = 1.0;         // data frames per device position frame
    std::unique_ptr<clocksync::RateConverter> converter_; // devRate_ != 48 kHz
    bool pendingDiscontinuity_ = false;
    HANDLE process_ = nullptr;      // process loopback: the target, to notice it exiting
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

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

struct ProcEntry {
    DWORD pid = 0, parent = 0;
    std::string exe;
};

std::map<DWORD, ProcEntry> processSnapshot() {
    std::map<DWORD, ProcEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        out[pe.th32ProcessID] = {pe.th32ProcessID, pe.th32ParentProcessID, narrow(pe.szExeFile)};
    CloseHandle(snap);
    return out;
}

// The root of pid's same-named tree: walk up while the parent has the same executable name.
DWORD rootOf(const std::map<DWORD, ProcEntry>& procs, DWORD pid) {
    std::set<DWORD> seen;
    for (;;) {
        auto it = procs.find(pid);
        if (it == procs.end() || !seen.insert(pid).second) return pid;
        auto parent = procs.find(it->second.parent);
        if (parent == procs.end() || lower(parent->second.exe) != lower(it->second.exe)) return pid;
        pid = parent->first;
    }
}

std::uint64_t creationTime(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return ~0ull;
    FILETIME c{}, e{}, k{}, u{};
    std::uint64_t t = ~0ull;
    if (GetProcessTimes(h, &c, &e, &k, &u)) t = (std::uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    CloseHandle(h);
    return t;
}

std::string windowTitle(DWORD pid) {
    struct Find {
        DWORD pid;
        std::wstring title;
    } find{pid, {}};
    EnumWindows(
        [](HWND w, LPARAM l) -> BOOL {
            auto* f = reinterpret_cast<Find*>(l);
            DWORD owner = 0;
            GetWindowThreadProcessId(w, &owner);
            if (owner != f->pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
            wchar_t buf[256];
            if (GetWindowTextW(w, buf, 256) > 0) {
                f->title = buf;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    return narrow(find.title.c_str());
}

class WasapiBackend final : public DeviceBackend {
public:
    explicit WasapiBackend(bool asio) {
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                         enumerator_.putVoid());
        if (enumerator_) {
            notifier_ = new NotificationClient([this] { changed(); });
            enumerator_->RegisterEndpointNotificationCallback(notifier_);
        }
        if (asio) asio_ = makeAsioHost([this] { changed(); });
    }
    ~WasapiBackend() override {
        asio_.reset();
        if (enumerator_ && notifier_) enumerator_->UnregisterEndpointNotificationCallback(notifier_);
        if (notifier_) notifier_->Release();
    }

    std::vector<DeviceInfo> enumerate() override {
        ComInit com;
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
        if (SUCCEEDED(enumerator_->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, col.put()))) {
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
                std::string iface;
                Com<IPropertyStore> props;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
                    PROPVARIANT v;
                    PropVariantInit(&v);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) info.name = narrow(v.pwszVal);
                    PropVariantClear(&v);
                    PropVariantInit(&v);
                    if (SUCCEEDED(props->GetValue(PKEY_DeviceInterface_FriendlyName, &v)) && v.vt == VT_LPWSTR) iface = narrow(v.pwszVal);
                    PropVariantClear(&v);
                    PropVariantInit(&v);
                    if (SUCCEEDED(props->GetValue(kDeviceFormatKey, &v)) && v.vt == VT_BLOB && v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
                        const auto* wf = reinterpret_cast<const WAVEFORMATEX*>(v.blob.pBlobData);
                        info.channels = wf->nChannels;
                        info.sampleRate = static_cast<int>(wf->nSamplesPerSec);
                    }
                    PropVariantClear(&v);
                }
                if (info.connected) {
                    // the mix format is what shared mode streams get
                    Com<IAudioClient> client;
                    if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid()))) {
                        WAVEFORMATEX* mix = nullptr;
                        if (SUCCEEDED(client->GetMixFormat(&mix))) {
                            info.channels = mix->nChannels;
                            if (info.sampleRate == 0) info.sampleRate = static_cast<int>(mix->nSamplesPerSec);
                            CoTaskMemFree(mix);
                        }
                    }
                }
                info.isDefault = info.id == defaults[info.input ? 1 : 0];
                // VB-Audio's cables ("CABLE Input (VB-Audio Virtual Cable)"), A+B and Voicemeeter's
                info.virtualCable = iface.find("VB-Audio") != std::string::npos || info.name.find("VB-Audio") != std::string::npos ||
                                    info.name.rfind("CABLE", 0) == 0;
                out.push_back(std::move(info));
            }
        }
        // stable order: defaults first, then by name
        std::stable_sort(out.begin(), out.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
            if (a.isDefault != b.isDefault) return a.isDefault;
            return a.name < b.name;
        });
        if (asio_)
            for (auto& d : asio_->enumerate()) out.push_back(std::move(d));
        return out;
    }

    void setChangeCallback(std::function<void()> cb) override {
        std::lock_guard lock(mutex_);
        cb_ = std::move(cb);
    }

    std::unique_ptr<Stream> openInput(const DeviceInfo& device, const StreamConfig& cfg, InputCallback cb,
                                      std::string* error) override {
        if (device.asio) return asio_ ? asio_->openInput(device, cfg, std::move(cb), error) : nullptr;
        auto s = std::make_unique<WasapiStream>(true, std::move(cb), nullptr, [this] { changed(); });
        if (!enumerator_ || !s->open(enumerator_.get(), device, cfg, error)) return nullptr;
        return s;
    }

    std::unique_ptr<Stream> openOutput(const DeviceInfo& device, const StreamConfig& cfg, OutputCallback cb,
                                       std::string* error) override {
        if (device.asio) return asio_ ? asio_->openOutput(device, cfg, std::move(cb), error) : nullptr;
        auto s = std::make_unique<WasapiStream>(false, nullptr, std::move(cb), [this] { changed(); });
        if (!enumerator_ || !s->open(enumerator_.get(), device, cfg, error)) return nullptr;
        return s;
    }

    std::vector<ProcessInfo> enumerateProcesses() override {
        ComInit com;
        const auto sessions = sessionPids();
        const auto procs = processSnapshot();
        std::map<std::string, std::pair<DWORD, std::uint64_t>> roots; // lower exe -> (root, creation time)
        for (DWORD pid : sessions) {
            if (!procs.count(pid)) continue;
            const DWORD root = rootOf(procs, pid);
            const auto key = lower(procs.at(root).exe);
            const auto created = creationTime(root);
            auto it = roots.find(key);
            if (it == roots.end() || created < it->second.second) roots[key] = {root, created};
        }
        std::vector<ProcessInfo> out;
        for (const auto& [key, root] : roots) out.push_back({procs.at(root.first).exe, root.first, windowTitle(root.first)});
        {
            std::lock_guard lock(mutex_);
            sessionPids_ = sessions;
        }
        return out;
    }

    ProcessInfo findProcess(const std::string& exe) override {
        const auto want = lower(exe);
        const auto procs = processSnapshot();
        std::set<DWORD> sessions;
        {
            std::lock_guard lock(mutex_);
            sessions = sessionPids_;
        }
        // roots of the trees with that name, and whether a process of the tree has an audio session
        std::map<DWORD, bool> roots;
        for (const auto& [pid, p] : procs)
            if (lower(p.exe) == want) roots[rootOf(procs, pid)] = false;
        for (DWORD pid : sessions)
            if (auto it = procs.find(pid); it != procs.end() && lower(it->second.exe) == want) roots[rootOf(procs, pid)] = true;
        DWORD best = 0;
        bool bestAudio = false;
        std::uint64_t bestTime = ~0ull;
        for (const auto& [pid, audio] : roots) {
            const auto t = creationTime(pid);
            if (best == 0 || (audio && !bestAudio) || (audio == bestAudio && t < bestTime)) {
                best = pid;
                bestAudio = audio;
                bestTime = t;
            }
        }
        if (best == 0) return {exe, 0, {}};
        return {procs.at(best).exe, best, windowTitle(best)};
    }

    std::unique_ptr<Stream> openProcess(const ProcessInfo& process, InputCallback cb, std::string* error) override {
        auto s = std::make_unique<WasapiStream>(true, std::move(cb), nullptr, [this] { changed(); });
        if (!s->openProcess(process, error)) return nullptr;
        return s;
    }

private:
    void changed() {
        std::lock_guard lock(mutex_);
        if (cb_) cb_();
    }

    // processes with an audio session (active or inactive) on any active render endpoint
    std::set<DWORD> sessionPids() {
        std::set<DWORD> out;
        if (!enumerator_) return out;
        Com<IMMDeviceCollection> col;
        if (FAILED(enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, col.put()))) return out;
        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            Com<IMMDevice> dev;
            Com<IAudioSessionManager2> mgr;
            Com<IAudioSessionEnumerator> sessions;
            if (FAILED(col->Item(i, dev.put())) ||
                FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, mgr.putVoid())) ||
                FAILED(mgr->GetSessionEnumerator(sessions.put())))
                continue;
            int n = 0;
            sessions->GetCount(&n);
            for (int k = 0; k < n; ++k) {
                Com<IAudioSessionControl> ctl;
                Com<IAudioSessionControl2> ctl2;
                if (FAILED(sessions->GetSession(k, ctl.put())) || FAILED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), ctl2.putVoid())))
                    continue;
                AudioSessionState state = AudioSessionStateExpired;
                ctl->GetState(&state);
                DWORD pid = 0;
                // 0 = the system sounds session
                if (state != AudioSessionStateExpired && SUCCEEDED(ctl2->GetProcessId(&pid)) && pid != 0 && pid != GetCurrentProcessId())
                    out.insert(pid);
            }
        }
        return out;
    }

    ComInit com_;
    Com<IMMDeviceEnumerator> enumerator_;
    NotificationClient* notifier_ = nullptr;
    std::unique_ptr<AsioHost> asio_;
    std::mutex mutex_;
    std::function<void()> cb_;
    std::set<DWORD> sessionPids_; // from the last enumerateProcesses()
};

} // namespace

std::unique_ptr<DeviceBackend> makeWasapiBackend(bool asio) { return std::make_unique<WasapiBackend>(asio); }

} // namespace dc::mixd
