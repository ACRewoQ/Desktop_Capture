#include "loopback_capture.hpp"

#include "wasapi_util.hpp"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <vector>

namespace v1 {

const char* modeName(LoopbackMode m) {
    switch (m) {
        case LoopbackMode::IncludeProcessTree: return "include-tree";
        case LoopbackMode::ExcludeProcessTree: return "exclude-tree";
        case LoopbackMode::DefaultDevice: return "device";
    }
    return "?";
}

namespace {

// ActivateAudioInterfaceAsync wants an agile completion handler; a free-threaded object that just signals.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivationHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ActivationHandler() { CloseHandle(done_); }

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
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
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const long n = --refs_;
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done_);
        return S_OK;
    }

    HANDLE done() const { return done_; }

private:
    std::atomic<long> refs_{1};
    HANDLE done_;
};

WAVEFORMATEX floatFormat() {
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = LoopbackCapture::kChannels;
    wf.nSamplesPerSec = LoopbackCapture::kRate;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = WORD(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    return wf;
}

WAVEFORMATEX pcm16Format() {
    WAVEFORMATEX wf = floatFormat();
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = WORD(wf.nChannels * 2);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    return wf;
}

HRESULT activateProcessLoopback(LoopbackMode mode, DWORD pid, IAudioClient** out, std::string* where) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = mode == LoopbackMode::IncludeProcessTree
                                                           ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                                                           : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof params;
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new ActivationHandler();
    Com<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler,
                                             op.put());
    if (FAILED(hr)) {
        handler->Release();
        *where = "ActivateAudioInterfaceAsync";
        return hr;
    }
    const DWORD wait = WaitForSingleObject(handler->done(), 5000);
    handler->Release();
    if (wait != WAIT_OBJECT_0) {
        *where = "activation timeout (5 s)";
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    HRESULT activateHr = E_FAIL;
    Com<IUnknown> unk;
    hr = op->GetActivateResult(&activateHr, unk.put());
    if (FAILED(hr) || FAILED(activateHr)) {
        *where = "GetActivateResult";
        return FAILED(hr) ? hr : activateHr;
    }
    hr = unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(out));
    if (FAILED(hr)) *where = "QueryInterface(IAudioClient)";
    return hr;
}

HRESULT activateDefaultRender(IAudioClient** out, std::string* where) {
    Com<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  enumerator.putVoid());
    if (FAILED(hr)) { *where = "CoCreateInstance(MMDeviceEnumerator)"; return hr; }
    Com<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
    if (FAILED(hr)) { *where = "GetDefaultAudioEndpoint"; return hr; }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(out));
    if (FAILED(hr)) *where = "IMMDevice::Activate";
    return hr;
}

}  // namespace

LoopbackCapture::~LoopbackCapture() {
    stop();
    if (capture_) capture_->Release();
    if (client_) client_->Release();
    if (event_) CloseHandle(event_);
}

HRESULT LoopbackCapture::open(LoopbackMode mode, DWORD pid, std::string* where) {
    std::string dummy;
    if (!where) where = &dummy;
    const auto t0 = nowHns();
    HRESULT hr = mode == LoopbackMode::DefaultDevice ? activateDefaultRender(&client_, where)
                                                     : activateProcessLoopback(mode, pid, &client_, where);
    if (FAILED(hr)) return hr;

    // Process loopback clients do not implement GetMixFormat / GetDevicePeriod, so the format is ours and
    // AUTOCONVERTPCM makes the engine convert. 20 ms buffer, event driven.
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    WAVEFORMATEX wf = floatFormat();
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &wf, nullptr);
    if (hr == static_cast<HRESULT>(0x88890008L) /* AUDCLNT_E_UNSUPPORTED_FORMAT */) {
        wf = pcm16Format();
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &wf, nullptr);
        pcm16_ = SUCCEEDED(hr);
    }
    if (FAILED(hr)) { *where = "IAudioClient::Initialize"; return hr; }

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = client_->SetEventHandle(event_);
    if (FAILED(hr)) { *where = "SetEventHandle"; return hr; }
    hr = client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_));
    if (FAILED(hr)) { *where = "GetService(IAudioCaptureClient)"; return hr; }
    activationMs_ = double(nowHns() - t0) / 10000.0;
    return S_OK;
}

HRESULT LoopbackCapture::start(Sink sink) {
    if (!client_) return E_UNEXPECTED;
    sink_ = std::move(sink);
    const HRESULT hr = client_->Start();
    if (FAILED(hr)) return hr;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
    return S_OK;
}

void LoopbackCapture::stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    SetEvent(event_);
    thread_.join();
    client_->Stop();
}

CaptureStats LoopbackCapture::stats() const {
    std::lock_guard lock(statsMutex_);
    return stats_;
}

void LoopbackCapture::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    std::vector<float> conv;
    std::uint64_t prevEndHns = 0;
    auto fail = [this](HRESULT hr) {
        std::lock_guard lock(statsMutex_);
        if (stats_.lastError == S_OK) stats_.lastError = hr;
    };

    while (!stop_) {
        WaitForSingleObject(event_, 100);
        for (;;) {
            UINT32 packet = 0;
            HRESULT hr = capture_->GetNextPacketSize(&packet);
            if (FAILED(hr)) { fail(hr); stop_ = true; break; }
            if (packet == 0) break;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD bufFlags = 0;
            UINT64 devPos = 0, qpc = 0;
            hr = capture_->GetBuffer(&data, &frames, &bufFlags, &devPos, &qpc);
            if (FAILED(hr)) { fail(hr); stop_ = true; break; }

            const size_t samples = size_t(frames) * kChannels;
            conv.resize(samples);
            if (bufFlags & AUDCLNT_BUFFERFLAGS_SILENT)
                std::fill(conv.begin(), conv.end(), 0.0f);
            else if (pcm16_) {
                const auto* s = reinterpret_cast<const std::int16_t*>(data);
                for (size_t i = 0; i < samples; ++i) conv[i] = float(s[i]) / 32768.0f;
            } else
                std::copy_n(reinterpret_cast<const float*>(data), samples, conv.begin());
            capture_->ReleaseBuffer(frames);

            const double packetMs = double(frames) * 1000.0 / kRate;
            {
                std::lock_guard lock(statsMutex_);
                stats_.frames += frames;
                ++stats_.packets;
                if (bufFlags & AUDCLNT_BUFFERFLAGS_SILENT) ++stats_.silentPackets;
                if (bufFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++stats_.discontinuities;
                stats_.maxPacketMs = std::max(stats_.maxPacketMs, packetMs);
                if (prevEndHns != 0 && qpc > prevEndHns)
                    stats_.maxGapMs = std::max(stats_.maxGapMs, double(qpc - prevEndHns) / 10000.0);
            }
            prevEndHns = qpc + std::uint64_t(frames) * 10'000'000ull / kRate;
            if (sink_) sink_(conv.data(), frames, qpc);
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

}  // namespace v1
