// ASIO host: one driver at a time on its own single-threaded apartment thread (drivers are COM
// objects that expect their calls from the thread that created them, with a window handle and a
// message loop), all channels opened at once, and every input / output stream of mixd fed from the
// driver's buffer switch callback.

#include "asio_backend.hpp"

#include "engine.hpp"

#include "dc/clocksync/rate_converter.hpp"

#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <thread>

namespace dc::mixd {

namespace {

// --- the ASIO driver interface (Steinberg ASIO SDK 2.3: asio.h and iasiodrv.h, dual licensed under
// the GPL v3). Only what the host uses is declared; the layout is the SDK's (4-byte packing, Windows
// long = 32 bits on x64 too).
#pragma pack(push, 4)
using ASIOBool = long;
using ASIOError = long;
using ASIOSampleRate = double;
using ASIOSampleType = long;
struct ASIOSamples {
    unsigned long hi, lo;
};
struct ASIOTimeStamp {
    unsigned long hi, lo;
};
struct ASIOClockSource {
    long index;
    long associatedChannel;
    long associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
};
struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long channelGroup;
    ASIOSampleType type;
    char name[32];
};
struct ASIOBufferInfo {
    ASIOBool isInput;
    long channelNum;
    void* buffers[2];
};
struct ASIOTime; // only passed through
struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
};
#pragma pack(pop)

struct IASIO : public IUnknown {
    virtual ASIOBool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                                    ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};

constexpr ASIOError kAseOk = 0;
enum : long {
    kAsioSelectorSupported = 1,
    kAsioEngineVersion = 2,
    kAsioResetRequest = 3,
    kAsioBufferSizeChange = 4,
    kAsioResyncRequest = 5,
    kAsioLatenciesChanged = 6,
    kAsioSupportsTimeInfo = 7,
    kAsioSupportsTimeCode = 8,
};
enum : long {
    kInt16LSB = 16,
    kInt24LSB = 17,
    kInt32LSB = 18,
    kFloat32LSB = 19,
    kFloat64LSB = 20,
    kInt32LSB16 = 24,
    kInt32LSB18 = 25,
    kInt32LSB20 = 26,
    kInt32LSB24 = 27,
};

constexpr int kMaxChannels = 64;
constexpr int kMaxPairs = kMaxChannels / 2;
constexpr double kReprobeSeconds = 10;

bool supportedType(ASIOSampleType t) {
    switch (t) {
        case kInt16LSB:
        case kInt24LSB:
        case kInt32LSB:
        case kFloat32LSB:
        case kFloat64LSB:
        case kInt32LSB16:
        case kInt32LSB18:
        case kInt32LSB20:
        case kInt32LSB24: return true;
        default: return false;
    }
}

int bytesOf(ASIOSampleType t) {
    switch (t) {
        case kInt16LSB: return 2;
        case kInt24LSB: return 3;
        case kFloat64LSB: return 8;
        default: return 4;
    }
}

// Scale of a 32-bit container whose value sits in the low bits (Int32LSB16 = 16 bit in 32).
double lowBitsScale(ASIOSampleType t) {
    switch (t) {
        case kInt32LSB16: return 32768.0;
        case kInt32LSB18: return 131072.0;
        case kInt32LSB20: return 524288.0;
        case kInt32LSB24: return 8388608.0;
        default: return 2147483648.0;
    }
}

// one channel of driver samples -> every `stride`-th float of dst
void toFloat(ASIOSampleType t, const void* src, float* dst, long frames, int stride) {
    switch (t) {
        case kInt16LSB: {
            const auto* s = static_cast<const std::int16_t*>(src);
            for (long i = 0; i < frames; ++i) dst[i * stride] = s[i] * (1.0f / 32768.0f);
            break;
        }
        case kInt24LSB: {
            const auto* b = static_cast<const unsigned char*>(src);
            for (long i = 0; i < frames; ++i) {
                const auto v = static_cast<std::int32_t>((std::uint32_t(b[3 * i]) << 8) | (std::uint32_t(b[3 * i + 1]) << 16) |
                                                         (std::uint32_t(b[3 * i + 2]) << 24));
                dst[i * stride] = static_cast<float>(v * (1.0 / 2147483648.0));
            }
            break;
        }
        case kFloat32LSB: {
            const auto* s = static_cast<const float*>(src);
            for (long i = 0; i < frames; ++i) dst[i * stride] = s[i];
            break;
        }
        case kFloat64LSB: {
            const auto* s = static_cast<const double*>(src);
            for (long i = 0; i < frames; ++i) dst[i * stride] = static_cast<float>(s[i]);
            break;
        }
        default: { // the 32-bit integer containers
            const auto* s = static_cast<const std::int32_t*>(src);
            const double k = 1.0 / lowBitsScale(t);
            for (long i = 0; i < frames; ++i) dst[i * stride] = static_cast<float>(s[i] * k);
            break;
        }
    }
}

// every `stride`-th float of src -> one channel of driver samples, clipped
void fromFloat(ASIOSampleType t, const float* src, int stride, void* dst, long frames) {
    auto clip = [](float v) { return std::clamp(static_cast<double>(v), -1.0, 1.0); };
    switch (t) {
        case kInt16LSB: {
            auto* d = static_cast<std::int16_t*>(dst);
            for (long i = 0; i < frames; ++i) d[i] = static_cast<std::int16_t>(clip(src[i * stride]) * 32767.0);
            break;
        }
        case kInt24LSB: {
            auto* b = static_cast<unsigned char*>(dst);
            for (long i = 0; i < frames; ++i) {
                const auto v = static_cast<std::int32_t>(clip(src[i * stride]) * 8388607.0);
                b[3 * i] = static_cast<unsigned char>(v & 0xff);
                b[3 * i + 1] = static_cast<unsigned char>((v >> 8) & 0xff);
                b[3 * i + 2] = static_cast<unsigned char>((v >> 16) & 0xff);
            }
            break;
        }
        case kFloat32LSB: {
            auto* d = static_cast<float*>(dst);
            for (long i = 0; i < frames; ++i) d[i] = src[i * stride];
            break;
        }
        case kFloat64LSB: {
            auto* d = static_cast<double*>(dst);
            for (long i = 0; i < frames; ++i) d[i] = src[i * stride];
            break;
        }
        default: {
            auto* d = static_cast<std::int32_t*>(dst);
            const double k = lowBitsScale(t) - 1.0;
            for (long i = 0; i < frames; ++i) d[i] = static_cast<std::int32_t>(clip(src[i * stride]) * k);
            break;
        }
    }
}

std::string narrowAnsi(const char* s) { return s ? std::string(s) : std::string(); }

// Runs every driver call on one STA thread with a hidden window, pumping its messages.
class DriverThread {
public:
    DriverThread() {
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::promise<void> ready;
        auto started = ready.get_future();
        thread_ = std::thread([this, &ready] { loop(ready); });
        started.wait();
    }
    ~DriverThread() {
        run([this] { stop_ = true; });
        thread_.join();
        CloseHandle(wake_);
    }

    template <typename F>
    auto run(F&& f) -> decltype(f()) {
        using R = decltype(f());
        if (std::this_thread::get_id() == thread_.get_id()) return f();
        std::packaged_task<R()> task(std::forward<F>(f));
        auto result = task.get_future();
        {
            std::lock_guard lock(mutex_);
            tasks_.emplace_back([&task] { task(); });
        }
        SetEvent(wake_);
        return result.get();
    }

    HWND window() const { return window_; }

private:
    void loop(std::promise<void>& ready) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // drivers get a window handle in init() (some parent their dialogs to it)
        window_ = CreateWindowExW(0, L"STATIC", L"dc-mixd-asio", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
        ready.set_value();
        while (!stop_) {
            MsgWaitForMultipleObjects(1, &wake_, FALSE, INFINITE, QS_ALLINPUT);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            std::deque<std::function<void()>> tasks;
            {
                std::lock_guard lock(mutex_);
                tasks.swap(tasks_);
            }
            for (auto& t : tasks) t();
        }
        if (window_) DestroyWindow(window_);
        CoUninitialize();
    }

    std::thread thread_;
    HANDLE wake_ = nullptr;
    HWND window_ = nullptr;
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    bool stop_ = false; // driver thread only
};

struct InSub {
    InputCallback cb;
    std::unique_ptr<clocksync::RateConverter> conv;
};

struct OutSub {
    OutputCallback cb;
    int first = 0; // first output channel (0-based)
    int count = 2; // 2, or 1 for the last channel of an odd count
    std::unique_ptr<clocksync::RateConverter> conv;
    std::vector<float> buf;
};

// The open driver. The callback side (process) runs on the driver's own audio thread.
struct Session {
    std::string driver;
    IASIO* drv = nullptr;
    long inputs = 0, outputs = 0, frames = 0;
    long inLatency = 0, outLatency = 0;
    double rate = kSampleRate;
    bool outputReady = false;
    std::vector<ASIOBufferInfo> buffers; // inputs, then outputs
    std::vector<ASIOSampleType> types;
    std::vector<float> inScratch;
    std::atomic<InSub*> in{nullptr};
    std::array<std::atomic<OutSub*>, kMaxPairs> out{};
    std::atomic<std::uint64_t> seq{0}; // odd while the callback runs
    std::atomic<bool> reset{false};
    std::atomic<bool> resync{false};
    std::atomic<std::uint64_t> glitches{0};
    double lastSwitch = 0;
    int users = 0; // streams open on it (host mutex)

    void process(long index) {
        seq.fetch_add(1, std::memory_order_acq_rel);
        const double now = qpcSecondsNow();
        if (lastSwitch > 0 && now - lastSwitch > 1.8 * static_cast<double>(frames) / rate) glitches.fetch_add(1);
        lastSwitch = now;
        if (InSub* sub = in.load(std::memory_order_acquire)) {
            for (long c = 0; c < inputs; ++c)
                toFloat(types[static_cast<std::size_t>(c)], buffers[static_cast<std::size_t>(c)].buffers[index], inScratch.data() + c,
                        frames, static_cast<int>(inputs));
            const bool disc = resync.exchange(false);
            const double t = now - static_cast<double>(frames + inLatency) / rate; // when the first frame was sampled
            if (sub->conv) {
                bool first = true;
                sub->conv->push(inScratch.data(), static_cast<std::size_t>(frames), t, [&](const float* o, std::size_t k, double ot) {
                    sub->cb(o, static_cast<int>(k), ot, disc && first);
                    first = false;
                });
            } else {
                sub->cb(inScratch.data(), static_cast<int>(frames), t, disc);
            }
        }
        std::array<bool, kMaxChannels> written{};
        const double when = now + static_cast<double>(outLatency) / rate; // when the new buffer is heard
        for (int p = 0; p < kMaxPairs; ++p) {
            OutSub* sub = out[static_cast<std::size_t>(p)].load(std::memory_order_acquire);
            if (!sub) continue;
            float* buf = sub->buf.data();
            if (sub->conv) {
                double supplied = 0;
                sub->conv->pull(buf, static_cast<std::size_t>(frames), [&](float* d, std::size_t k) {
                    sub->cb(d, static_cast<int>(k), sub->count, when + supplied / kSampleRate);
                    supplied += static_cast<double>(k);
                });
            } else {
                sub->cb(buf, static_cast<int>(frames), sub->count, when);
            }
            for (int c = 0; c < sub->count; ++c) {
                const auto ch = static_cast<std::size_t>(inputs + sub->first + c);
                fromFloat(types[ch], buf + c, sub->count, buffers[ch].buffers[index], frames);
                written[static_cast<std::size_t>(sub->first + c)] = true;
            }
        }
        for (long o = 0; o < outputs; ++o)
            if (!written[static_cast<std::size_t>(o)]) {
                const auto ch = static_cast<std::size_t>(inputs + o);
                std::memset(buffers[ch].buffers[index], 0, static_cast<std::size_t>(frames) * static_cast<std::size_t>(bytesOf(types[ch])));
            }
        if (outputReady) drv->outputReady();
        seq.fetch_add(1, std::memory_order_release);
    }

    // returns once a callback that may still see a removed subscriber has finished
    void waitForCallback() const {
        const auto v = seq.load(std::memory_order_acquire);
        if ((v & 1) == 0) return;
        for (int i = 0; i < 500 && seq.load(std::memory_order_acquire) == v; ++i) Sleep(1);
    }
};

std::atomic<Session*> g_session{nullptr};
std::function<void()>* g_onChange = nullptr;

void cbBufferSwitch(long index, ASIOBool) {
    if (Session* s = g_session.load(std::memory_order_acquire)) s->process(index);
}
ASIOTime* cbBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct) {
    cbBufferSwitch(index, direct);
    return params;
}
void requestReset() {
    if (Session* s = g_session.load()) s->reset = true;
    if (g_onChange && *g_onChange) (*g_onChange)();
}
void cbSampleRateDidChange(ASIOSampleRate) { requestReset(); }
long cbAsioMessage(long selector, long value, void*, double*) {
    switch (selector) {
        case kAsioSelectorSupported:
            return value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioBufferSizeChange ||
                           value == kAsioResyncRequest || value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo
                       ? 1
                       : 0;
        case kAsioEngineVersion: return 2;
        case kAsioResetRequest:
        case kAsioBufferSizeChange: requestReset(); return 1;
        case kAsioResyncRequest:
            if (Session* s = g_session.load()) s->resync = true;
            return 1;
        case kAsioLatenciesChanged: return 1;
        case kAsioSupportsTimeInfo: return 1;
        case kAsioSupportsTimeCode: return 0;
        default: return 0;
    }
}
ASIOCallbacks g_callbacks{cbBufferSwitch, cbSampleRateDidChange, cbAsioMessage, cbBufferSwitchTimeInfo};

struct DriverInfo {
    CLSID clsid{};
    bool probed = false;
    bool ok = false;
    double probedAt = 0;
    long inputs = 0, outputs = 0;
    int rate = 0; // the rate mixd will run it at: 48000 when it can, else its own
};

std::string driverError(IASIO* drv, const char* what) {
    char msg[256] = {};
    if (drv) drv->getErrorMessage(msg);
    return std::string(what) + (msg[0] ? std::string(": ") + narrowAnsi(msg) : std::string());
}

class HostImpl;

class AsioStream final : public Stream {
public:
    AsioStream(HostImpl& host, Session& session, std::unique_ptr<InSub> in, std::unique_ptr<OutSub> out, int pair, bool countGlitches)
        : host_(host), session_(session), in_(std::move(in)), out_(std::move(out)), pair_(pair), countGlitches_(countGlitches),
          glitchBase_(session.glitches.load()) {}
    ~AsioStream() override;

    bool start(std::string*) override {
        if (in_) session_.in.store(in_.get(), std::memory_order_release);
        if (out_) session_.out[static_cast<std::size_t>(pair_)].store(out_.get(), std::memory_order_release);
        return true;
    }
    void stop() override;
    int channels() const override { return in_ ? static_cast<int>(session_.inputs) : out_->count; }
    int periodFrames() const override {
        return static_cast<int>(std::lround(static_cast<double>(session_.frames) * kSampleRate / session_.rate));
    }
    double latencySeconds() const override {
        return static_cast<double>(in_ ? session_.inLatency : session_.outLatency) / session_.rate;
    }
    bool failed() const override { return session_.reset.load(); }
    std::uint64_t glitches() const override { return countGlitches_ ? session_.glitches.load() - glitchBase_ : 0; }
    std::string diagnostics() const override {
        return ", asio " + session_.driver + " buffer " + std::to_string(session_.frames) + " at " +
               std::to_string(static_cast<int>(session_.rate)) + " Hz";
    }

private:
    HostImpl& host_;
    Session& session_;
    std::unique_ptr<InSub> in_;
    std::unique_ptr<OutSub> out_;
    int pair_;
    bool countGlitches_;
    std::uint64_t glitchBase_;
    bool stopped_ = false;
};

class HostImpl final : public AsioHost {
public:
    explicit HostImpl(std::function<void()> onChange) : onChange_(std::move(onChange)) { g_onChange = &onChange_; }
    ~HostImpl() override {
        // streams are gone by now; close the driver if one is still open
        std::lock_guard lock(mutex_);
        if (session_) closeSession();
        g_onChange = nullptr;
    }

    std::vector<DeviceInfo> enumerate() override {
        std::lock_guard lock(mutex_);
        const auto registry = readRegistry();
        std::vector<DeviceInfo> out;
        for (const auto& [name, clsid] : registry) {
            auto& info = drivers_[name];
            info.clsid = clsid;
            const bool open = session_ && session_->driver == name;
            if (open) {
                info.ok = true;
                info.inputs = session_->inputs;
                info.outputs = session_->outputs;
                info.rate = static_cast<int>(session_->rate);
            } else if (!info.probed || (!info.ok && qpcSecondsNow() - info.probedAt > kReprobeSeconds)) {
                probe(info);
            }
            DeviceInfo in;
            in.id = "asio:" + name;
            in.name = name;
            in.input = true;
            in.channels = static_cast<int>(info.inputs);
            in.connected = info.ok && info.inputs > 0;
            in.asio = true;
            in.sampleRate = info.rate;
            out.push_back(in);
            for (long c = 0; c < info.outputs || (!info.ok && c == 0); c += 2) {
                const long last = std::min(c + 1, std::max(info.outputs, 2L) - 1);
                const std::string range = std::to_string(c + 1) + "-" + std::to_string(last + 1);
                DeviceInfo o;
                o.id = "asio:" + name + ":" + range;
                o.name = name + " Out " + range;
                o.channels = static_cast<int>(last - c + 1);
                o.connected = info.ok && info.outputs > 0;
                o.asio = true;
                o.sampleRate = info.rate;
                out.push_back(o);
                if (!info.ok) break;
            }
        }
        return out;
    }

    std::unique_ptr<Stream> openInput(const DeviceInfo& device, const StreamConfig& cfg, InputCallback cb,
                                      std::string* error) override {
        std::lock_guard lock(mutex_);
        const std::string name = device.id.substr(5);
        Session* s = session(name, cfg, error);
        if (!s) return nullptr;
        if (s->in.load()) return fail(error, "input already open");
        auto sub = std::make_unique<InSub>();
        sub->cb = std::move(cb);
        if (s->rate != kSampleRate)
            sub->conv = std::make_unique<clocksync::RateConverter>(static_cast<int>(s->inputs), s->rate, kSampleRate,
                                                                   static_cast<std::size_t>(s->frames));
        return made(*s, std::move(sub), nullptr, 0);
    }

    std::unique_ptr<Stream> openOutput(const DeviceInfo& device, const StreamConfig& cfg, OutputCallback cb,
                                       std::string* error) override {
        std::lock_guard lock(mutex_);
        // "asio:<driver>:<a>-<b>"
        const auto colon = device.id.rfind(':');
        const auto dash = device.id.rfind('-');
        if (colon == std::string::npos || colon < 5 || dash == std::string::npos || dash < colon)
            return fail(error, "bad ASIO output id");
        const std::string name = device.id.substr(5, colon - 5);
        const int a = std::atoi(device.id.c_str() + colon + 1) - 1;
        const int b = std::atoi(device.id.c_str() + dash + 1) - 1;
        Session* s = session(name, cfg, error);
        if (!s) return nullptr;
        if (a < 0 || b < a || b >= s->outputs || a / 2 >= kMaxPairs) return fail(error, "no such ASIO output channels");
        if (s->out[static_cast<std::size_t>(a / 2)].load()) return fail(error, "output already open");
        auto sub = std::make_unique<OutSub>();
        sub->cb = std::move(cb);
        sub->first = a;
        sub->count = b - a + 1;
        sub->buf.assign(static_cast<std::size_t>(s->frames) * static_cast<std::size_t>(sub->count), 0.0f);
        if (s->rate != kSampleRate)
            sub->conv = std::make_unique<clocksync::RateConverter>(sub->count, kSampleRate, s->rate, static_cast<std::size_t>(s->frames));
        return made(*s, nullptr, std::move(sub), a / 2);
    }

    // AsioStream::stop
    void release(Session& s) {
        std::lock_guard lock(mutex_);
        if (--s.users == 0 && session_.get() == &s) closeSession();
    }

private:
    std::unique_ptr<Stream> fail(std::string* error, const char* what) {
        if (error) *error = what;
        return nullptr;
    }

    std::unique_ptr<Stream> made(Session& s, std::unique_ptr<InSub> in, std::unique_ptr<OutSub> out, int pair) {
        const bool first = s.users == 0;
        ++s.users;
        return std::make_unique<AsioStream>(*this, s, std::move(in), std::move(out), pair, first);
    }

    static std::map<std::string, CLSID> readRegistry() {
        std::map<std::string, CLSID> out;
        HKEY root = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return out;
        char name[256];
        for (DWORD i = 0;; ++i) {
            DWORD len = sizeof name;
            if (RegEnumKeyExA(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            char value[128] = {};
            DWORD size = sizeof value;
            if (RegGetValueA(root, name, "CLSID", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) continue;
            wchar_t wide[128] = {};
            MultiByteToWideChar(CP_ACP, 0, value, -1, wide, 128);
            CLSID clsid{};
            if (SUCCEEDED(CLSIDFromString(wide, &clsid))) out[name] = clsid;
        }
        RegCloseKey(root);
        return out;
    }

    // loads the driver once to read its channels (driver thread)
    void probe(DriverInfo& info) {
        info.probed = true;
        info.probedAt = qpcSecondsNow();
        thread_.run([&] {
            IASIO* drv = nullptr;
            // an ASIO driver's interface id is its class id
            if (FAILED(CoCreateInstance(info.clsid, nullptr, CLSCTX_INPROC_SERVER, info.clsid, reinterpret_cast<void**>(&drv))) || !drv) {
                info.ok = false;
                return;
            }
            info.ok = drv->init(thread_.window()) != 0 && drv->getChannels(&info.inputs, &info.outputs) == kAseOk;
            if (info.ok) {
                info.inputs = std::min<long>(info.inputs, kMaxChannels);
                info.outputs = std::min<long>(info.outputs, kMaxChannels);
                ASIOSampleRate rate = 0;
                drv->getSampleRate(&rate);
                info.rate = drv->canSampleRate(kSampleRate) == kAseOk ? static_cast<int>(kSampleRate) : static_cast<int>(rate);
            }
            drv->Release();
        });
    }

    // the open session for this driver, opening it if none is open (host mutex held)
    Session* session(const std::string& name, const StreamConfig& cfg, std::string* error) {
        if (session_) {
            if (session_->driver == name && !session_->reset) return session_.get();
            if (session_->driver != name) {
                if (error) *error = "ASIO は同時に 1 つのドライバーしか開けない（" + session_->driver + " を使用中）";
                return nullptr;
            }
            return nullptr; // reset requested: wait until its streams are closed
        }
        auto it = drivers_.find(name);
        if (it == drivers_.end()) {
            const auto registry = readRegistry();
            auto r = registry.find(name);
            if (r == registry.end()) {
                if (error) *error = "ASIO driver not installed";
                return nullptr;
            }
            it = drivers_.emplace(name, DriverInfo{r->second}).first;
        }
        auto s = std::make_unique<Session>();
        s->driver = name;
        std::string err;
        const bool ok = thread_.run([&] { return openSession(*s, it->second.clsid, cfg, &err); });
        if (!ok) {
            if (error) *error = err;
            return nullptr;
        }
        session_ = std::move(s);
        return session_.get();
    }

    // driver thread
    bool openSession(Session& s, const CLSID& clsid, const StreamConfig& cfg, std::string* error) {
        IASIO* drv = nullptr;
        if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&drv))) || !drv) {
            *error = "ASIO driver could not be loaded";
            return false;
        }
        s.drv = drv;
        auto bail = [&](const std::string& what) {
            *error = what;
            drv->Release();
            s.drv = nullptr;
            return false;
        };
        if (!drv->init(thread_.window())) return bail(driverError(drv, "init"));
        if (drv->getChannels(&s.inputs, &s.outputs) != kAseOk) return bail(driverError(drv, "getChannels"));
        s.inputs = std::min<long>(s.inputs, kMaxChannels);
        s.outputs = std::min<long>(s.outputs, kMaxChannels);

        if (drv->canSampleRate(kSampleRate) == kAseOk) drv->setSampleRate(kSampleRate);
        ASIOSampleRate rate = 0;
        if (drv->getSampleRate(&rate) != kAseOk || rate <= 0) return bail(driverError(drv, "getSampleRate"));
        s.rate = rate;

        long mn = 0, mx = 0, pref = 0, gran = 0;
        if (drv->getBufferSize(&mn, &mx, &pref, &gran) != kAseOk || pref <= 0) return bail(driverError(drv, "getBufferSize"));
        // the block size if the driver allows it, else the size set in its control panel
        const long want = cfg.blockFrames;
        bool allowed = want >= mn && want <= mx;
        if (allowed && gran == -1) allowed = (want & (want - 1)) == 0;
        else if (allowed && gran > 0) allowed = (want - mn) % gran == 0;
        else if (allowed && gran == 0) allowed = want == pref;
        s.frames = allowed ? want : pref;

        const long total = s.inputs + s.outputs;
        s.buffers.resize(static_cast<std::size_t>(total));
        for (long c = 0; c < total; ++c) {
            auto& b = s.buffers[static_cast<std::size_t>(c)];
            b.isInput = c < s.inputs ? 1 : 0;
            b.channelNum = c < s.inputs ? c : c - s.inputs;
            b.buffers[0] = b.buffers[1] = nullptr;
        }
        g_session.store(&s, std::memory_order_release);
        if (drv->createBuffers(s.buffers.data(), total, s.frames, &g_callbacks) != kAseOk) {
            g_session.store(nullptr);
            return bail(driverError(drv, "createBuffers"));
        }
        s.types.resize(static_cast<std::size_t>(total));
        for (long c = 0; c < total; ++c) {
            ASIOChannelInfo ci{};
            ci.channel = s.buffers[static_cast<std::size_t>(c)].channelNum;
            ci.isInput = s.buffers[static_cast<std::size_t>(c)].isInput;
            drv->getChannelInfo(&ci);
            if (!supportedType(ci.type)) {
                drv->disposeBuffers();
                g_session.store(nullptr);
                return bail("unsupported ASIO sample type " + std::to_string(ci.type));
            }
            s.types[static_cast<std::size_t>(c)] = ci.type;
        }
        drv->getLatencies(&s.inLatency, &s.outLatency);
        s.outputReady = drv->outputReady() == kAseOk;
        s.inScratch.assign(static_cast<std::size_t>(std::max(s.inputs, 1L)) * static_cast<std::size_t>(s.frames), 0.0f);
        for (long c = s.inputs; c < total; ++c)
            for (int half = 0; half < 2; ++half)
                std::memset(s.buffers[static_cast<std::size_t>(c)].buffers[half], 0,
                            static_cast<std::size_t>(s.frames) * static_cast<std::size_t>(bytesOf(s.types[static_cast<std::size_t>(c)])));
        if (drv->start() != kAseOk) {
            drv->disposeBuffers();
            g_session.store(nullptr);
            return bail(driverError(drv, "start"));
        }
        return true;
    }

    // host mutex held
    void closeSession() {
        Session* s = session_.get();
        thread_.run([s] {
            s->drv->stop();
            g_session.store(nullptr, std::memory_order_release);
            s->drv->disposeBuffers();
            s->drv->Release();
            s->drv = nullptr;
        });
        session_.reset();
    }

    std::function<void()> onChange_;
    DriverThread thread_;
    std::mutex mutex_;
    std::map<std::string, DriverInfo> drivers_;
    std::unique_ptr<Session> session_;
};

AsioStream::~AsioStream() { stop(); }

void AsioStream::stop() {
    if (stopped_) return;
    stopped_ = true;
    if (in_ && session_.in.load() == in_.get()) session_.in.store(nullptr, std::memory_order_release);
    if (out_ && session_.out[static_cast<std::size_t>(pair_)].load() == out_.get())
        session_.out[static_cast<std::size_t>(pair_)].store(nullptr, std::memory_order_release);
    session_.waitForCallback();
    host_.release(session_);
}

} // namespace

std::unique_ptr<AsioHost> makeAsioHost(std::function<void()> onChange) { return std::make_unique<HostImpl>(std::move(onChange)); }

} // namespace dc::mixd
