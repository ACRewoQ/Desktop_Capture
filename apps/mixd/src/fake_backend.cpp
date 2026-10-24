#include "fake_backend.hpp"

#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace dc::mixd {

namespace {

class FakeStream final : public Stream {
public:
    FakeStream(FakeBackend::Device device, const StreamConfig& cfg, InputCallback in, OutputCallback out,
               std::shared_ptr<FakeBackend::Meter> meter, std::shared_ptr<std::atomic<bool>> alive = nullptr)
        : alive_(std::move(alive)), dev_(std::move(device)), fallback_(cfg.exclusive && dev_.sharedOnly),
          wrongPeriod_(cfg.exclusive && !fallback_ && dev_.exclusivePeriod > cfg.blockFrames),
          period_(cfg.exclusive && !fallback_ ? cfg.blockFrames : dev_.periodFrames), in_(std::move(in)), out_(std::move(out)),
          meter_(std::move(meter)) {}
    ~FakeStream() override { stop(); }

    bool start(std::string*) override {
        stop_ = false;
        started_ = qpcSecondsNow();
        thread_ = std::thread([this] { run(); });
        return true;
    }
    void stop() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    int channels() const override { return dev_.info.channels; }
    int periodFrames() const override { return period_; }
    double latencySeconds() const override { return period_ / kSampleRate; }
    bool failed() const override { return suggestedPeriod() > 0 || (alive_ && !alive_->load()); }
    std::uint64_t glitches() const override { return 0; }
    bool sharedFallback() const override { return fallback_; }
    // like the UR22: an exclusive period smaller than the device's own fails after a moment
    int suggestedPeriod() const override { return wrongPeriod_ && started_.load() > 0 && qpcSecondsNow() - started_.load() > 0.2 ? dev_.exclusivePeriod : 0; }

private:
    void run() {
        const int ch = std::max(1, dev_.info.channels);
        std::vector<float> buf(static_cast<std::size_t>(period_ * ch));
        const double rate = kSampleRate * (1.0 + dev_.ppm * 1e-6);
        const double t0 = qpcSecondsNow();
        std::uint64_t done = 0;
        double phase = 0;
        const double step = 2 * kPi * dev_.toneHz / kSampleRate;
        while (!stop_) {
            const double due = t0 + static_cast<double>(done + static_cast<std::uint64_t>(period_)) / rate;
            const double wait = due - qpcSecondsNow();
            if (wait > 0) {
                std::this_thread::sleep_for(std::chrono::duration<double>(std::min(wait, 0.05)));
                continue;
            }
            const double time = t0 + static_cast<double>(done) / rate;
            if (in_) {
                for (int i = 0; i < period_; ++i) {
                    const float v = dev_.toneLevel * static_cast<float>(std::sin(phase));
                    phase += step;
                    for (int c = 0; c < ch; ++c) buf[static_cast<std::size_t>(i * ch + c)] = v;
                }
                phase = std::fmod(phase, 2 * kPi);
                in_(buf.data(), period_, time, false);
            } else {
                out_(buf.data(), period_, ch, time);
                float peak = 0;
                for (float v : buf) peak = std::max(peak, std::fabs(v));
                float cur = meter_->peak.load();
                while (peak > cur && !meter_->peak.compare_exchange_weak(cur, peak)) {
                }
                meter_->frames.fetch_add(static_cast<std::uint64_t>(period_));
            }
            done += static_cast<std::uint64_t>(period_);
        }
    }

    std::shared_ptr<std::atomic<bool>> alive_; // processes: false once the application ended
    FakeBackend::Device dev_;
    bool fallback_;
    bool wrongPeriod_;
    int period_;
    std::atomic<double> started_{0};
    InputCallback in_;
    OutputCallback out_;
    std::shared_ptr<FakeBackend::Meter> meter_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

std::string lowerName(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

} // namespace

std::vector<FakeBackend::Device> FakeBackend::standardDevices() {
    auto dev = [](const char* id, const char* name, bool input, int channels, bool isDefault, double ppm, double tone) {
        Device d;
        d.info.id = id;
        d.info.name = name;
        d.info.input = input;
        d.info.channels = channels;
        d.info.connected = true;
        d.info.isDefault = isDefault;
        d.ppm = ppm;
        d.toneHz = tone;
        return d;
    };
    return {
        dev("fake-mic", "Fake Microphone", true, 1, true, 30, 440),
        dev("fake-line", "Fake Line In", true, 2, false, -20, 1000),
        dev("fake-spk", "Fake Speakers", false, 2, true, 0, 0),
        dev("fake-hp", "Fake Headphones", false, 2, false, 45, 0),
    };
}

FakeBackend::FakeBackend(std::vector<Device> devices, std::vector<Process> processes)
    : devices_(std::move(devices)), processes_(std::move(processes)) {
    for (const auto& d : devices_)
        if (!d.info.input) meters_[d.info.id] = std::make_shared<Meter>();
}

FakeBackend::~FakeBackend() = default;

void FakeBackend::setConnected(const std::string& id, bool connected) {
    std::function<void()> cb;
    {
        std::lock_guard lock(mutex_);
        for (auto& d : devices_)
            if (d.info.id == id) d.info.connected = connected;
        cb = cb_;
    }
    if (cb) cb();
}

void FakeBackend::changeId(const std::string& id, const std::string& newId) {
    std::function<void()> cb;
    {
        std::lock_guard lock(mutex_);
        for (auto& d : devices_)
            if (d.info.id == id) d.info.id = newId;
        if (auto it = meters_.find(id); it != meters_.end()) {
            meters_[newId] = it->second;
            meters_.erase(it);
        }
        cb = cb_;
    }
    if (cb) cb();
}

void FakeBackend::setProcessRunning(const std::string& exe, bool running, std::uint32_t pid) {
    std::lock_guard lock(mutex_);
    for (auto& p : processes_) {
        if (p.info.exe != exe) continue;
        if (p.running && !running) p.alive->store(false);
        if (!p.running && running) p.alive = std::make_shared<std::atomic<bool>>(true);
        p.running = running;
        if (pid) p.info.pid = pid;
    }
}

std::vector<ProcessInfo> FakeBackend::enumerateProcesses() {
    std::lock_guard lock(mutex_);
    std::vector<ProcessInfo> out;
    for (const auto& p : processes_)
        if (p.running) out.push_back(p.info);
    return out;
}

ProcessInfo FakeBackend::findProcess(const std::string& exe) {
    std::lock_guard lock(mutex_);
    for (const auto& p : processes_)
        if (p.running && lowerName(p.info.exe) == lowerName(exe)) return p.info;
    return {exe, 0, {}};
}

std::unique_ptr<Stream> FakeBackend::openProcess(const ProcessInfo& process, InputCallback cb, std::string* error) {
    std::lock_guard lock(mutex_);
    for (const auto& p : processes_)
        if (p.running && p.info.pid == process.pid) {
            Device d;
            d.info.id = "pid-" + std::to_string(p.info.pid);
            d.info.name = p.info.exe;
            d.info.input = true;
            d.info.channels = 2;
            d.toneHz = p.toneHz;
            return std::make_unique<FakeStream>(d, StreamConfig{}, std::move(cb), nullptr, nullptr, p.alive);
        }
    if (error) *error = "no such process";
    return nullptr;
}

float FakeBackend::takeOutputPeak(const std::string& id) {
    std::lock_guard lock(mutex_);
    auto it = meters_.find(id);
    return it == meters_.end() ? 0.0f : it->second->peak.exchange(0);
}

std::uint64_t FakeBackend::outputFrames(const std::string& id) {
    std::lock_guard lock(mutex_);
    auto it = meters_.find(id);
    return it == meters_.end() ? 0 : it->second->frames.load();
}

std::vector<DeviceInfo> FakeBackend::enumerate() {
    std::lock_guard lock(mutex_);
    std::vector<DeviceInfo> out;
    for (const auto& d : devices_) out.push_back(d.info);
    return out;
}

void FakeBackend::setChangeCallback(std::function<void()> cb) {
    std::lock_guard lock(mutex_);
    cb_ = std::move(cb);
}

std::unique_ptr<Stream> FakeBackend::openInput(const DeviceInfo& device, const StreamConfig& cfg, InputCallback cb,
                                               std::string* error) {
    std::lock_guard lock(mutex_);
    for (const auto& d : devices_)
        if (d.info.id == device.id && d.info.input && d.info.connected)
            return std::make_unique<FakeStream>(d, cfg, std::move(cb), nullptr,
                                                nullptr);
    if (error) *error = "no such device";
    return nullptr;
}

std::unique_ptr<Stream> FakeBackend::openOutput(const DeviceInfo& device, const StreamConfig& cfg, OutputCallback cb,
                                                std::string* error) {
    std::lock_guard lock(mutex_);
    for (const auto& d : devices_)
        if (d.info.id == device.id && !d.info.input && d.info.connected)
            return std::make_unique<FakeStream>(d, cfg, nullptr, std::move(cb),
                                                meters_[d.info.id]);
    if (error) *error = "no such device";
    return nullptr;
}

} // namespace dc::mixd
