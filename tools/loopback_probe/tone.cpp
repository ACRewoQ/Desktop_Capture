#include "tone.hpp"

#include "wasapi_util.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

namespace v1 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kFadeFrames = kRate / 100;  // 10 ms fade in / out, keeps the spectrum clean

int failed(const char* step, HRESULT hr) {
    std::cerr << "tone: " << step << " failed: " << hrText(hr) << "\n";
    return 2;
}
}  // namespace

int playTone(const ToneOptions& opt) {
    Com<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  enumerator.putVoid());
    if (FAILED(hr)) return failed("CoCreateInstance", hr);
    Com<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
    if (FAILED(hr)) return failed("GetDefaultAudioEndpoint", hr);
    Com<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
    if (FAILED(hr)) return failed("Activate", hr);

    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = 2;
    wf.nSamplesPerSec = kRate;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = 8;
    wf.nAvgBytesPerSec = kRate * 8;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            200000, 0, &wf, nullptr);
    if (FAILED(hr)) return failed("Initialize", hr);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    client->SetEventHandle(ev);
    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);
    Com<IAudioRenderClient> render;
    hr = client->GetService(__uuidof(IAudioRenderClient), render.putVoid());
    if (FAILED(hr)) return failed("GetService", hr);

    const std::uint64_t delayFrames = std::uint64_t(opt.delayMs) * kRate / 1000;
    const std::uint64_t toneFrames = std::uint64_t(opt.seconds * kRate);
    const std::uint64_t totalFrames = delayFrames + toneFrames;
    std::uint64_t pos = 0;
    bool reported = false;

    auto fill = [&](UINT32 frames) -> HRESULT {
        BYTE* data = nullptr;
        HRESULT r = render->GetBuffer(frames, &data);
        if (FAILED(r)) return r;
        auto* out = reinterpret_cast<float*>(data);
        bool any = false;
        for (UINT32 i = 0; i < frames; ++i, ++pos) {
            float s = 0.0f;
            if (pos >= delayFrames && pos < totalFrames) {
                const std::uint64_t n = pos - delayFrames;
                double env = 1.0;
                if (n < kFadeFrames) env = double(n) / kFadeFrames;
                if (toneFrames - n < kFadeFrames) env = double(toneFrames - n) / kFadeFrames;
                s = static_cast<float>(opt.amplitude * env * std::sin(2.0 * kPi * opt.freq * double(n) / kRate));
                any = true;
            }
            out[2 * i] = out[2 * i + 1] = s;
        }
        r = render->ReleaseBuffer(frames, 0);
        if (any && !reported && !opt.reportPath.empty()) {
            std::ofstream(opt.reportPath) << nowHns() << "\n";
            reported = true;
        }
        return r;
    };

    hr = fill(bufferFrames);
    if (FAILED(hr)) return failed("first GetBuffer", hr);
    hr = client->Start();
    if (FAILED(hr)) return failed("Start", hr);
    while (pos < totalFrames + kRate / 10) {  // run 100 ms of silence past the end so the tail is played
        WaitForSingleObject(ev, 200);
        UINT32 padding = 0;
        hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr)) return failed("GetCurrentPadding", hr);
        if (bufferFrames > padding) {
            hr = fill(bufferFrames - padding);
            if (FAILED(hr)) return failed("GetBuffer", hr);
        }
    }
    client->Stop();
    CloseHandle(ev);
    return 0;
}

}  // namespace v1
