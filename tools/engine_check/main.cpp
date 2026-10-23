// dc-engine-check: listens to what an output device really plays (WASAPI loopback, shared mode) and
// checks that a continuous test tone has no dropouts (gate G2 "バッファ128〜256で音切れなし").
//
//   dc-engine-check [--device ID] [--seconds 600] [--tone 1000] [--warmup 5]
//
// Start mixd with --test-tone 1000 first; the tone goes through the master to its outputs. Every
// 100 ms block of the loopback gets a sine fit (clocksync::ToneAnalyzer); a block whose SINAD falls
// under 60 dB or whose level drops out is a glitch. Other sounds on the same device (notifications)
// also count, so keep the device quiet during the test. Exclusive mode streams bypass the loopback:
// for those, mixd's /system/xruns is the measure.
//
// Prints one summary line: "blocks N glitches G min_sinad_db S mean_sinad_db M".

#include "dc/clocksync/tone_analyzer.hpp"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

template <typename T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() {
        if (p) p->Release();
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

bool check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "%s failed: 0x%08lx\n", what, static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

} // namespace

int main(int argc, char** argv) {
    std::string device;
    double seconds = 600, toneHz = 1000, warmup = 5;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--device") device = next();
        else if (a == "--seconds") seconds = std::stod(next());
        else if (a == "--tone") toneHz = std::stod(next());
        else if (a == "--warmup") warmup = std::stod(next());
        else {
            std::fprintf(stderr, "usage: dc-engine-check [--device ID] [--seconds 600] [--tone 1000] [--warmup 5]\n");
            return 2;
        }
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Com<IMMDeviceEnumerator> en;
    if (!check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(en.put())),
               "CoCreateInstance"))
        return 1;
    Com<IMMDevice> dev;
    if (device.empty()) {
        if (!check(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put()), "GetDefaultAudioEndpoint")) return 1;
    } else if (!check(en->GetDevice(widen(device).c_str(), dev.put()), "GetDevice")) {
        return 1;
    }
    Com<IAudioClient> client;
    if (!check(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put())), "Activate"))
        return 1;
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = 2;
    f.Format.nSamplesPerSec = 48000;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = 8;
    f.Format.nAvgBytesPerSec = 48000 * 8;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f.SubFormat = kSubtypeFloat;
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (!check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, &f.Format, nullptr), "Initialize")) return 1;
    Com<IAudioCaptureClient> capture;
    if (!check(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture.put())), "GetService"))
        return 1;
    if (!check(client->Start(), "Start")) return 1;

    dc::clocksync::ToneAnalyzer::Config cfg;
    cfg.toneHz = toneHz;
    dc::clocksync::ToneAnalyzer analyzer(cfg);
    const auto start = std::chrono::steady_clock::now();
    bool warm = false;
    std::uint64_t discontinuities = 0, packets = 0;
    std::vector<double> events; // seconds after warm-up
    while (true) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (t >= warmup + seconds) break;
        if (!warm && t >= warmup) {
            analyzer.resetSummary();
            warm = true;
        }
        UINT32 next = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&next)) && next > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD fl = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &fl, nullptr, nullptr))) break;
            if ((fl & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && packets > 0 && warm) ++discontinuities;
            ++packets;
            std::vector<float> zeros;
            const float* samples = reinterpret_cast<const float*>(data);
            if (fl & AUDCLNT_BUFFERFLAGS_SILENT) {
                zeros.assign(static_cast<std::size_t>(frames) * 2, 0.0f);
                samples = zeros.data();
            }
            analyzer.feed(samples, frames, 2, [&](const dc::clocksync::ToneAnalyzer::Block& b) {
                if (warm && b.glitch) {
                    if (events.size() < 50) events.push_back(t - warmup);
                    std::printf("glitch at %.1f s: level %.4f sinad %.1f dB\n", t - warmup, b.amplitude, b.sinadDb);
                    std::fflush(stdout);
                }
            });
            capture->ReleaseBuffer(frames);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    client->Stop();
    const auto& s = analyzer.summary();
    std::printf("blocks %llu glitches %llu min_sinad_db %.1f mean_sinad_db %.1f loopback_discontinuities %llu\n",
                static_cast<unsigned long long>(s.blocks), static_cast<unsigned long long>(s.glitchBlocks),
                s.blocks ? s.minSinadDb : 0.0, s.meanSinadDb, static_cast<unsigned long long>(discontinuities));
    return 0;
}
