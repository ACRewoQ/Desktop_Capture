// dc-drift-test: clock drift correction between two real audio devices (verification V2, Windows).
//
//   dc-drift-test --list
//   dc-drift-test [--a N] [--b N] [--minutes 60] [--tone 1000] [--level -20] [--target-ms 30] [--csv FILE]
//
// Device A and device B are two render endpoints (speakers, HDMI, USB DAC, virtual cable...) that run
// on their own clocks. The test builds the same path the engine will use for a second device:
//
//   tone -> render A -> loopback capture A (clocked by A) -> ClockBridge -> render B
//                                                                 \-> loopback capture B -> analysis
//
// The loopback of B is what B really played. It is checked for clicks (a sine fit per 100 ms block) and
// for latency drift (tone phase vs. the time A produced it), while the bridge reports underruns,
// overruns and the clock difference it measured. A quiet tone is audible on both devices; turn the
// speakers down (the loopback is taken before the speakers) and avoid other sounds on A and B.

#include "dc/clocksync/clock_bridge.hpp"
#include "dc/clocksync/time_dll.hpp"
#include "dc/clocksync/tone_analyzer.hpp"

#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
// defines PKEY_Device_FriendlyName here instead of needing it from a library
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace dc::clocksync;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000;
constexpr int kChannels = 2;
constexpr REFERENCE_TIME kRenderBuffer = 200000;   // 20 ms
constexpr REFERENCE_TIME kLoopbackBuffer = 1000000; // 100 ms
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out to avoid ksmedia.h
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

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(std::max(n, 1)) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

double qpcSeconds() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return double(v.QuadPart) / freq;
}

WAVEFORMATEXTENSIBLE floatFormat() {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = kChannels;
    f.Format.nSamplesPerSec = DWORD(kRate);
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = kChannels * 4;
    f.Format.nAvgBytesPerSec = DWORD(kRate) * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f.SubFormat = kSubtypeFloat;
    return f;
}

struct DeviceInfo {
    std::wstring id;
    std::string name;
    int mixRate = 0;
    int mixChannels = 0;
    double periodMs = 0;
};

std::vector<DeviceInfo> listRenderDevices(IMMDeviceEnumerator* en, std::wstring* defaultId) {
    std::vector<DeviceInfo> out;
    Com<IMMDevice> def;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, def.put()))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) {
            *defaultId = id;
            CoTaskMemFree(id);
        }
    }
    Com<IMMDeviceCollection> col;
    if (!check(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, col.put()), "EnumAudioEndpoints")) return out;
    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        Com<IMMDevice> dev;
        if (FAILED(col->Item(i, dev.put()))) continue;
        DeviceInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) {
            info.id = id;
            CoTaskMemFree(id);
        }
        Com<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                info.name = narrow(v.pwszVal);
            PropVariantClear(&v);
        }
        Com<IAudioClient> client;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put())))) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix))) {
                info.mixRate = int(mix->nSamplesPerSec);
                info.mixChannels = mix->nChannels;
                CoTaskMemFree(mix);
            }
            REFERENCE_TIME defPeriod = 0, minPeriod = 0;
            if (SUCCEEDED(client->GetDevicePeriod(&defPeriod, &minPeriod))) info.periodMs = double(defPeriod) / 10000;
        }
        out.push_back(info);
    }
    return out;
}

// One WASAPI shared-mode stream on a render endpoint: either rendering (event driven) or capturing its
// loopback (polled). The format is always 48 kHz float stereo; Windows converts to the mix format.
class Stream {
public:
    bool open(IMMDeviceEnumerator* en, const std::wstring& id, bool loopback) {
        loopback_ = loopback;
        Com<IMMDevice> dev;
        if (!check(en->GetDevice(id.c_str(), dev.put()), "GetDevice")) return false;
        if (!check(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_)),
                   "Activate"))
            return false;
        auto fmt = floatFormat();
        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        flags |= loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (!check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, loopback ? kLoopbackBuffer : kRenderBuffer, 0,
                                       &fmt.Format, nullptr),
                   "IAudioClient::Initialize"))
            return false;
        client_->GetBufferSize(&bufferFrames_);
        if (loopback) {
            if (!check(client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_)),
                       "GetService(capture)"))
                return false;
        } else {
            event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            client_->SetEventHandle(event_);
            if (!check(client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_)),
                       "GetService(render)"))
                return false;
            client_->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&clock_));
            if (clock_) clock_->GetFrequency(&clockFreq_);
        }
        return true;
    }

    ~Stream() {
        if (client_) client_->Stop();
        if (capture_) capture_->Release();
        if (render_) render_->Release();
        if (clock_) clock_->Release();
        if (client_) client_->Release();
        if (event_) CloseHandle(event_);
    }

    bool start() { return check(client_->Start(), "IAudioClient::Start"); }

    // Render: waits for the device, then asks fill(buffer, frames, timeOfFirstFrame) for the free space.
    template <typename F> bool renderOnce(F&& fill) {
        if (WaitForSingleObject(event_, 200) != WAIT_OBJECT_0) return true; // keep going; device may be idle
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) return false;
        const UINT32 frames = bufferFrames_ - padding;
        if (frames == 0) return true;
        BYTE* data = nullptr;
        if (FAILED(render_->GetBuffer(frames, &data))) return false;
        // When will the first new frame play? Device position at a QPC instant, plus what is queued.
        double when = qpcSeconds() + double(padding) / kRate;
        if (clock_ && clockFreq_) {
            UINT64 pos = 0, qpc = 0;
            if (SUCCEEDED(clock_->GetPosition(&pos, &qpc)))
                when = double(qpc) * 1e-7 + (double(written_) - double(pos) / double(clockFreq_) * kRate) / kRate;
        }
        fill(reinterpret_cast<float*>(data), std::size_t(frames), when);
        written_ += frames;
        return SUCCEEDED(render_->ReleaseBuffer(frames, 0));
    }

    struct Packet {
        const float* data;
        std::size_t frames;
        UINT64 devicePosition;
        double time; // when the first frame was played on the device (QPC seconds)
        bool discontinuity;
        bool silent;
    };

    // Loopback: hands every waiting packet to onPacket.
    template <typename F> bool captureAvailable(F&& onPacket) {
        UINT32 next = 0;
        while (SUCCEEDED(capture_->GetNextPacketSize(&next)) && next > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devPos = 0, qpc = 0;
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, &devPos, &qpc))) return false;
            onPacket(Packet{reinterpret_cast<const float*>(data), frames, devPos, double(qpc) * 1e-7,
                            (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0,
                            (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0});
            if (FAILED(capture_->ReleaseBuffer(frames))) return false;
        }
        return true;
    }

private:
    bool loopback_ = false;
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    IAudioClock* clock_ = nullptr;
    UINT64 clockFreq_ = 0;
    UINT32 bufferFrames_ = 0;
    UINT64 written_ = 0;
    HANDLE event_ = nullptr;
};

struct Options {
    int a = -1, b = -1;
    double minutes = 60;
    double toneHz = 1000;
    double levelDb = -20;
    double targetMs = 30;
    double warmupSec = 60;
    std::string csv;
    bool list = false;
};

// Shared between the threads; everything below the atomics is guarded by mutex.
struct Shared {
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> warm{false};
    std::mutex mutex;
    // Loopback positions count device frames at the device's mix rate (44.1 kHz on some devices), not
    // the 48 kHz stream frames, so these run at the mix rates; see run().
    TimeDll dllA{kRate, 0.1}; // A loopback: device frame -> time
    TimeDll dllB{kRate, 0.1}; // B loopback
    double mixRateA = kRate, mixRateB = kRate;
    ClockBridge::Stats atWarm; // bridge counters when the warm-up ended
    std::uint64_t packetsA = 0, packetsB = 0;
    std::uint64_t discontA = 0, discontB = 0;
    ToneAnalyzer::Summary toneA, toneB;
    double latencyMs = 0, latMin = 1e9, latMax = -1e9;
    double intervalMinSinadB = 1e9;
    bool haveLatency = false;
    // last clean block of A's loopback: where a tone frame sits in A's device frames
    bool haveA = false;
    double lastAPos = 0, lastASrc = 0;
    std::vector<double> aGlitchTimes; // QPC seconds of A glitch blocks (other sounds on A)
    std::uint64_t glitchBOwn = 0;     // B glitch blocks with no A glitch around them: the bridge's
    std::vector<std::pair<double, bool>> bEvents; // B glitch blocks: seconds after warm-up, caused by A
    std::vector<double> pendingB; // B glitch blocks (play time) waiting for A's analysis to catch up

    // Decides for each pending B glitch whether A had a glitch around it. A's blocks finish on their
    // own schedule, so a B block is judged only after A has certainly analysed that moment.
    void resolveB(double now, bool all) {
        std::erase_if(pendingB, [&](double played) {
            if (!all && now < played + 2.5) return false;
            bool fromA = false;
            for (double t : aGlitchTimes) fromA = fromA || (t > played - 2.0 && t < played + 1.0);
            if (!fromA) ++glitchBOwn;
            if (bEvents.size() < 200) bEvents.push_back({played - warmTime, fromA});
            return true;
        });
    }
    double warmTime = 0, warmPosA = 0, warmPosB = 0;
};

void proAudioThread() {
    DWORD task = 0;
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
}

void fail(Shared& sh, const char* what) {
    std::fprintf(stderr, "stream error in %s, stopping\n", what);
    sh.failed = true;
    sh.stop = true;
}

int run(const Options& opt) {
    Com<IMMDeviceEnumerator> en;
    if (!check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(en.put())),
               "MMDeviceEnumerator"))
        return 1;
    std::wstring defaultId;
    const auto devices = listRenderDevices(en.p, &defaultId);

    if (opt.list) {
        for (std::size_t i = 0; i < devices.size(); ++i)
            std::printf("%2zu%s %s  (mix %d Hz, %d ch, period %.2f ms)\n", i, devices[i].id == defaultId ? "*" : " ",
                        devices[i].name.c_str(), devices[i].mixRate, devices[i].mixChannels, devices[i].periodMs);
        std::printf("* = default output\n");
        return 0;
    }
    if (devices.size() < 2) {
        std::fprintf(stderr, "need two active output devices, found %zu\n", devices.size());
        return 1;
    }
    int ia = opt.a, ib = opt.b;
    if (ia < 0)
        for (std::size_t i = 0; i < devices.size(); ++i)
            if (devices[i].id == defaultId) ia = int(i);
    if (ia < 0) ia = 0;
    if (ib < 0) ib = ia == 0 ? 1 : 0;
    if (ia >= int(devices.size()) || ib >= int(devices.size()) || ia == ib) {
        std::fprintf(stderr, "bad device choice (--a %d --b %d); see --list\n", ia, ib);
        return 2;
    }
    const auto& devA = devices[std::size_t(ia)];
    const auto& devB = devices[std::size_t(ib)];
    std::printf("A (source): %s  [%d Hz, period %.2f ms]\n", devA.name.c_str(), devA.mixRate, devA.periodMs);
    std::printf("B (output): %s  [%d Hz, period %.2f ms]\n", devB.name.c_str(), devB.mixRate, devB.periodMs);
    std::printf("tone %.0f Hz at %.0f dBFS, target latency %.0f ms, %.0f min (+ %.0f s warm-up excluded)\n",
                opt.toneHz, opt.levelDb, opt.targetMs, opt.minutes, opt.warmupSec);

    std::string csvPath = opt.csv;
    if (csvPath.empty()) {
        char buf[64];
        const std::time_t now = std::time(nullptr);
        std::strftime(buf, sizeof buf, "drift_test_%Y%m%d_%H%M%S.csv", std::localtime(&now));
        csvPath = buf;
    }
    FILE* csv = std::fopen(csvPath.c_str(), "w");
    if (!csv) {
        std::fprintf(stderr, "cannot write %s\n", csvPath.c_str());
        return 1;
    }
    std::fprintf(csv, "elapsed_s,measured_ppm,bridge_ppm,fill_error_frames,fifo_frames,latency_ms,underruns,overruns,"
                      "resyncs,discont_a,discont_b,glitch_blocks_a,glitch_blocks_b,glitch_blocks_b_own,min_sinad_b_db\n");

    Stream renderA, loopA, renderB, loopB;
    if (!renderA.open(en.p, devA.id, false) || !loopA.open(en.p, devA.id, true) || !renderB.open(en.p, devB.id, false) ||
        !loopB.open(en.p, devB.id, true))
        return 1;

    ClockBridge::Config bc;
    bc.channels = kChannels;
    bc.nominalRate = kRate;
    bc.targetFrames = std::size_t(opt.targetMs * kRate / 1000);
    bc.fifoFrames = 32768;
    bc.maxBlockFrames = 8192;
    ClockBridge bridge(bc);

    Shared sh;
    sh.mixRateA = devA.mixRate > 0 ? devA.mixRate : kRate;
    sh.mixRateB = devB.mixRate > 0 ? devB.mixRate : kRate;
    sh.dllA = TimeDll(sh.mixRateA, 0.1);
    sh.dllB = TimeDll(sh.mixRateB, 0.1);
    // clock difference in ppm, independent of the nominal mix rates
    // Clock difference in ppm, independent of the nominal mix rates: frames each device advanced since
    // the warm-up ended (long baseline, so timestamp noise averages out), or the DLL rates before that.
    auto measuredPpm = [&] {
        if (!sh.dllA.ready() || !sh.dllB.ready()) return 0.0;
        const double t = qpcSeconds();
        if (sh.warm && t - sh.warmTime > 5) {
            const double a = (sh.dllA.positionAt(t) - sh.warmPosA) / sh.mixRateA;
            const double b = (sh.dllB.positionAt(t) - sh.warmPosB) / sh.mixRateB;
            return (a / b - 1) * 1e6;
        }
        return (sh.dllA.rate() / sh.mixRateA) / (sh.dllB.rate() / sh.mixRateB) * 1e6 - 1e6;
    };
    const double amp = std::pow(10.0, opt.levelDb / 20);
    ToneAnalyzer::Config ac;
    ac.toneHz = opt.toneHz;
    ac.minAmplitude = amp * 0.25;
    const double omega0 = 2 * kPi * opt.toneHz / kRate;

    // Thread 1: the tone into A (clocked by A).
    std::thread tRenderA([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        proAudioThread();
        std::uint64_t n = 0;
        auto fill = [&](float* out, std::size_t frames, double) {
            for (std::size_t i = 0; i < frames; ++i, ++n) {
                const double cyc = std::fmod(double(n) * opt.toneHz / kRate, 1.0);
                const float s = float(amp * std::sin(2 * kPi * cyc));
                out[2 * i] = s;
                out[2 * i + 1] = s;
            }
        };
        if (!renderA.renderOnce(fill) || !renderA.start()) return fail(sh, "render A");
        while (!sh.stop)
            if (!renderA.renderOnce(fill)) return fail(sh, "render A");
    });

    // Thread 2: A's loopback (what A played, at A's pace) into the bridge.
    std::thread tLoopA([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        proAudioThread();
        ToneAnalyzer ana(ac);
        bool first = true, warm = false;
        std::uint64_t basePos = 0; // device position of analyzer frame 0
        bool haveBase = false;
        std::vector<float> zeros;
        if (!loopA.start()) return fail(sh, "loopback A");
        while (!sh.stop) {
            Sleep(2);
            const bool ok = loopA.captureAvailable([&](const Stream::Packet& p) {
                const float* data = p.data;
                if (p.silent) {
                    zeros.assign(p.frames * kChannels, 0.0f);
                    data = zeros.data();
                }
                if (p.discontinuity && !first) bridge.producerDiscontinuity();
                bridge.push(data, p.frames, p.time);
                std::lock_guard lock(sh.mutex);
                if (!haveBase) {
                    basePos = p.devicePosition;
                    haveBase = true;
                }
                if (p.discontinuity && !first) {
                    ++sh.discontA;
                    ana.resetTracking();
                    sh.haveA = false;
                }
                sh.dllA.update(p.devicePosition, p.time);
                ++sh.packetsA;
                if (!warm && sh.warm) {
                    warm = true;
                    ana.resetSummary();
                }
                ana.feed(data, p.frames, kChannels, [&](const ToneAnalyzer::Block& b) {
                    const double pos = double(basePos) + double(b.startFrame) * sh.mixRateA / kRate;
                    if (b.glitch) {
                        if (sh.dllA.ready() && sh.aGlitchTimes.size() < 100000)
                            sh.aGlitchTimes.push_back(sh.dllA.timeAt(pos));
                        return;
                    }
                    sh.lastAPos = pos;
                    sh.lastASrc = (b.unwrappedPhase + kPi / 2) / omega0;
                    sh.haveA = true;
                });
                sh.toneA = ana.summary();
                first = false;
            });
            if (!ok) return fail(sh, "loopback A");
        }
    });

    // Thread 3: the bridge output into B (clocked by B).
    std::thread tRenderB([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        proAudioThread();
        auto fill = [&](float* out, std::size_t frames, double when) { bridge.pull(out, frames, when); };
        if (!renderB.renderOnce(fill) || !renderB.start()) return fail(sh, "render B");
        while (!sh.stop)
            if (!renderB.renderOnce(fill)) return fail(sh, "render B");
    });

    // Thread 4: B's loopback (what B really played): clicks and latency drift.
    std::thread tLoopB([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ToneAnalyzer ana(ac);
        bool first = true, warm = false;
        std::uint64_t basePos = 0; // device position of analyzer frame 0
        bool haveBase = false;
        std::vector<float> zeros;
        if (!loopB.start()) return fail(sh, "loopback B");
        while (!sh.stop) {
            Sleep(2);
            const bool ok = loopB.captureAvailable([&](const Stream::Packet& p) {
                const float* data = p.data;
                if (p.silent) {
                    zeros.assign(p.frames * kChannels, 0.0f);
                    data = zeros.data();
                }
                std::lock_guard lock(sh.mutex);
                if (!haveBase) {
                    basePos = p.devicePosition;
                    haveBase = true;
                }
                if (p.discontinuity && !first) {
                    ++sh.discontB;
                    ana.resetTracking();
                }
                first = false;
                sh.dllB.update(p.devicePosition, p.time);
                ++sh.packetsB;
                if (!warm && sh.warm) {
                    warm = true;
                    ana.resetSummary();
                }
                ana.feed(data, p.frames, kChannels, [&](const ToneAnalyzer::Block& b) {
                    if (!sh.warm) return;
                    sh.intervalMinSinadB = std::min(sh.intervalMinSinadB, b.sinadDb);
                    if (!sh.dllA.ready() || !sh.dllB.ready()) return;
                    const double played =
                        sh.dllB.timeAt(double(basePos) + double(b.startFrame) * sh.mixRateB / kRate);
                    if (b.glitch) {
                        // a glitch that A also had (another sound on A) is not the bridge's; see resolveB
                        sh.pendingB.push_back(played);
                        return;
                    }
                    if (!sh.haveA) return;
                    // The tone phase names the tone frame being played (up to whole tone periods).
                    // A's last clean block tells where tone frames sit in A's device frames, so the
                    // DLL only extrapolates over the latency. Tone and analyzer frames are 48 kHz
                    // stream frames, the DLLs count device frames at the mix rate.
                    const double srcFrame = (b.unwrappedPhase + kPi / 2) / omega0;
                    const double posA = sh.lastAPos + (srcFrame - sh.lastASrc) * sh.mixRateA / kRate;
                    const double produced = sh.dllA.timeAt(posA);
                    // Restarted phase tracking (after a glitch) shifts the reading by whole tone periods;
                    // unwrap against the previous reading so only a real change remains.
                    const double raw = (played - produced) * 1000;
                    const double periodMs = 1000 / opt.toneHz;
                    sh.latencyMs = sh.haveLatency
                                       ? sh.latencyMs + (raw - sh.latencyMs) -
                                             periodMs * std::round((raw - sh.latencyMs) / periodMs)
                                       : raw;
                    sh.latMin = std::min(sh.latMin, sh.latencyMs);
                    sh.latMax = std::max(sh.latMax, sh.latencyMs);
                    sh.haveLatency = true;
                });
                sh.toneB = ana.summary();
                sh.resolveB(p.time, false);
            });
            if (!ok) return fail(sh, "loopback B");
        }
    });

    const double t0 = qpcSeconds();
    const double total = opt.warmupSec + opt.minutes * 60;
    double nextLog = 10;
    int exitCode = 0;
    while (!sh.stop) {
        Sleep(200);
        const double el = qpcSeconds() - t0;
        if (!sh.warm && el >= opt.warmupSec) {
            std::lock_guard lock(sh.mutex);
            sh.warm = true;
            sh.atWarm = bridge.stats();
            sh.warmTime = qpcSeconds();
            sh.warmPosA = sh.dllA.positionAt(sh.warmTime);
            sh.warmPosB = sh.dllB.positionAt(sh.warmTime);
            sh.latMin = 1e9;
            sh.latMax = -1e9;
        }
        if (el >= nextLog || el >= total) {
            nextLog += 10;
            const auto st = bridge.stats();
            std::lock_guard lock(sh.mutex);
            const double clocksPpm = measuredPpm();
            const double bridgePpm = (st.ratio - 1) * 1e6;
            const double sinad = sh.intervalMinSinadB < 1e8 ? sh.intervalMinSinadB : 0;
            std::printf("%6.0f s %s clocks %+8.2f ppm  bridge %+8.2f ppm  fill %+6.1f  fifo %5.0f  latency %7.3f ms  "
                        "u/o/r %llu/%llu/%llu  glitch A %llu B %llu (own %llu)  SINAD %.0f dB\n",
                        el, sh.warm ? " " : "w", clocksPpm, bridgePpm, st.fillError, st.fifoFrames, sh.latencyMs,
                        (unsigned long long)st.underruns, (unsigned long long)st.overruns,
                        (unsigned long long)st.resyncs, (unsigned long long)sh.toneA.glitchBlocks,
                        (unsigned long long)sh.toneB.glitchBlocks, (unsigned long long)sh.glitchBOwn, sinad);
            std::fprintf(csv, "%.1f,%.3f,%.3f,%.2f,%.0f,%.4f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.1f\n", el, clocksPpm,
                         bridgePpm, st.fillError, st.fifoFrames, sh.latencyMs, (unsigned long long)st.underruns,
                         (unsigned long long)st.overruns, (unsigned long long)st.resyncs,
                         (unsigned long long)sh.discontA, (unsigned long long)sh.discontB,
                         (unsigned long long)sh.toneA.glitchBlocks, (unsigned long long)sh.toneB.glitchBlocks,
                         (unsigned long long)sh.glitchBOwn, sinad);
            std::fflush(csv);
            std::fflush(stdout);
            sh.intervalMinSinadB = 1e9;
            if (el >= opt.warmupSec + 20 && sh.packetsB == 0) {
                std::fprintf(stderr, "no loopback data from B: is the device really playing?\n");
                exitCode = 1;
                sh.stop = true;
            }
        }
        if (el >= total) sh.stop = true;
    }
    tRenderA.join();
    tLoopA.join();
    tRenderB.join();
    tLoopB.join();
    std::fclose(csv);

    auto st = bridge.stats();
    std::lock_guard lock(sh.mutex);
    sh.resolveB(0, true);
    // bridge events during the warm-up (device start-up) are reported but not judged
    const auto warmUnder = sh.atWarm.underruns, warmOver = sh.atWarm.overruns, warmResync = sh.atWarm.resyncs;
    st.underruns -= warmUnder;
    st.overruns -= warmOver;
    st.resyncs -= warmResync;
    const double drift = sh.latMax >= sh.latMin ? sh.latMax - sh.latMin : -1;
    const bool pass = !sh.failed && exitCode == 0 && st.underruns == 0 && st.overruns == 0 && st.resyncs == 0 &&
                      sh.toneB.blocks > 0 && sh.glitchBOwn == 0 && sh.discontB == 0 && drift >= 0 &&
                      drift < 1.0;
    std::printf("\n=== summary (after warm-up) ===\n");
    std::printf("A: %s\nB: %s\n", devA.name.c_str(), devB.name.c_str());
    std::printf("clock difference measured %+.2f ppm, bridge ratio %+.2f ppm\n", measuredPpm(),
                (st.ratio - 1) * 1e6);
    std::printf("bridge: underruns %llu, overruns %llu, resyncs %llu (warm-up: %llu/%llu/%llu, not judged)\n",
                (unsigned long long)st.underruns, (unsigned long long)st.overruns, (unsigned long long)st.resyncs,
                (unsigned long long)warmUnder, (unsigned long long)warmOver, (unsigned long long)warmResync);
    std::printf("B output: %llu blocks, %llu glitch blocks (%llu not explained by A), min SINAD %.1f dB, mean %.1f dB, "
                "discontinuities %llu\n",
                (unsigned long long)sh.toneB.blocks, (unsigned long long)sh.toneB.glitchBlocks,
                (unsigned long long)sh.glitchBOwn, sh.toneB.minSinadDb, sh.toneB.meanSinadDb,
                (unsigned long long)sh.discontB);
    std::printf("A source: %llu glitch blocks, discontinuities %llu (problems on A are not the bridge's)\n",
                (unsigned long long)sh.toneA.glitchBlocks, (unsigned long long)sh.discontA);
    std::printf("latency drift (max - min): %.3f ms\n", drift);
    for (const auto& [at, fromA] : sh.bEvents)
        std::printf("  B glitch at %.1f s after warm-up%s\n", at, fromA ? " (A had one too: other sound on A)" : "");
    std::printf("csv: %s\n", csvPath.c_str());
    std::printf("%s\n", pass ? "V2 PASS" : "V2 FAIL");
    return pass ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? argv[++i] : "0"; };
        if (a == "--list") opt.list = true;
        else if (a == "--a") opt.a = std::atoi(next());
        else if (a == "--b") opt.b = std::atoi(next());
        else if (a == "--minutes") opt.minutes = std::atof(next());
        else if (a == "--tone") opt.toneHz = std::atof(next());
        else if (a == "--level") opt.levelDb = std::atof(next());
        else if (a == "--target-ms") opt.targetMs = std::atof(next());
        else if (a == "--warmup") opt.warmupSec = std::atof(next());
        else if (a == "--csv") opt.csv = next();
        else {
            std::fprintf(stderr, "usage: dc-drift-test --list | [--a N] [--b N] [--minutes 60] [--tone 1000] "
                                 "[--level -20] [--target-ms 30] [--warmup 60] [--csv FILE]\n");
            return 2;
        }
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const int rc = run(opt);
    CoUninitialize();
    return rc;
}
