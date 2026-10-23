#include "device_manager.hpp"

#include <algorithm>
#include <chrono>

namespace dc::mixd {

namespace {

constexpr double kRetrySeconds = 3.0;
constexpr int kSharedPeriod = 480; // WASAPI shared mode engine period (10 ms)

std::size_t nextPow2(std::size_t v) {
    std::size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

} // namespace

DeviceManager::DeviceManager(DeviceBackend& backend, Engine& engine, Options options)
    : backend_(backend), engine_(engine), options_(options) {}

DeviceManager::~DeviceManager() { stop(); }

void DeviceManager::start(std::function<void(const State&)> listener) {
    listener_ = std::move(listener);
    backend_.setChangeCallback([this] { refresh(); });
    thread_ = std::thread([this] { run(); });
}

void DeviceManager::stop() {
    {
        std::lock_guard lock(mutex_);
        if (stop_) return;
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    backend_.setChangeCallback(nullptr);
    std::vector<std::string> keys;
    for (auto& [k, s] : streams_) keys.push_back(k);
    for (auto& k : keys) closeStream(k);
    if (timer_) timer_->stop();
    timer_.reset();
}

void DeviceManager::setDemand(const DeviceDemand& demand) {
    {
        std::lock_guard lock(mutex_);
        if (demand == demand_ && !demandChanged_) return;
        demand_ = demand;
        demandChanged_ = true;
        wake_ = true;
    }
    cv_.notify_all();
}

void DeviceManager::refresh() {
    {
        std::lock_guard lock(mutex_);
        refresh_ = true;
        wake_ = true;
    }
    cv_.notify_all();
}

std::uint64_t DeviceManager::glitches() const {
    std::lock_guard lock(statsMutex_);
    std::uint64_t n = closedGlitches_;
    for (auto* s : statStreams_) n += s->glitches();
    for (auto& p : statInputs_) {
        const auto st = p->bridge.stats();
        n += st.underruns + st.overruns;
    }
    for (auto& p : statOutputs_) {
        const auto st = p->bridge.stats();
        n += st.underruns + st.overruns;
    }
    return n;
}

void DeviceManager::run() {
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            // the periodic wake-up retries devices that failed to open and notices failed streams
            cv_.wait_for(lock, std::chrono::seconds(1), [&] { return wake_ || stop_; });
            if (stop_) return;
            wake_ = false;
        }
        reconcile();
    }
}

int DeviceManager::clockPeriod() const {
    if (!clockKey_.empty())
        if (auto it = streams_.find(clockKey_); it != streams_.end()) return it->second.stream->periodFrames();
    return config_.exclusive ? config_.blockFrames : kSharedPeriod;
}

clocksync::ClockBridge::Config DeviceManager::bridgeConfig(int channels, int devicePeriod) const {
    clocksync::ClockBridge::Config c;
    c.channels = channels;
    // room for one period of each side twice over plus 2 ms; V2 showed USB and virtual devices
    // sometimes stall for tens of ms, --bridge-ms raises the floor for those
    const int clock = config_.exclusive ? config_.blockFrames : kSharedPeriod;
    const double target = std::max(2.0 * (devicePeriod + std::max(clock, clockPeriod())) + 96.0,
                                   options_.bridgeMinMs * kSampleRate / 1000.0);
    c.targetFrames = static_cast<std::size_t>(target);
    c.fifoFrames = nextPow2(std::max<std::size_t>(16384, c.targetFrames * 4));
    c.maxBlockFrames = 8192;
    return c;
}

bool DeviceManager::retryAllowed(const std::string& key) {
    auto it = lastFailure_.find(key);
    return it == lastFailure_.end() || qpcSecondsNow() - it->second >= kRetrySeconds;
}

void DeviceManager::accountClosed(const OpenStream& s) {
    std::lock_guard lock(statsMutex_);
    closedGlitches_ += s.stream->glitches();
    if (s.input) {
        const auto st = s.input->bridge.stats();
        closedGlitches_ += st.underruns + st.overruns;
        std::erase(statInputs_, s.input);
    }
    if (s.output) {
        const auto st = s.output->bridge.stats();
        closedGlitches_ += st.underruns + st.overruns;
        std::erase(statOutputs_, s.output);
    }
    std::erase(statStreams_, s.stream.get());
}

void DeviceManager::closeStream(const std::string& key) {
    auto it = streams_.find(key);
    if (it == streams_.end()) return;
    it->second.stream->stop();
    if (it->second.input) it->second.input->streaming = false;
    if (it->second.output) it->second.output->streaming = false;
    accountClosed(it->second);
    if (key == clockKey_) clockKey_.clear();
    streams_.erase(it);
}

bool DeviceManager::openInput(const DeviceInfo& d, std::string* error) {
    // the bridge needs the device period, known only once the stream is open: let the callback reach
    // the port through a holder filled in before start()
    auto holder = std::make_shared<std::shared_ptr<InputPort>>();
    auto stream = backend_.openInput(
        d, config_,
        [holder](const float* frames, int count, double time, bool discontinuity) {
            auto& port = *holder;
            if (discontinuity) port->bridge.producerDiscontinuity();
            port->bridge.push(frames, static_cast<std::size_t>(count), time);
        },
        error);
    if (!stream) return false;
    *holder = std::make_shared<InputPort>(d.id, stream->channels(), bridgeConfig(stream->channels(), stream->periodFrames()));
    if (!stream->start(error)) return false;
    (*holder)->streaming = true;
    OpenStream s{std::move(stream), d, *holder, nullptr, false};
    {
        std::lock_guard lock(statsMutex_);
        statStreams_.push_back(s.stream.get());
        statInputs_.push_back(s.input);
    }
    streams_["in:" + d.id] = std::move(s);
    return true;
}

bool DeviceManager::openOutput(const DeviceInfo& d, bool asClock, std::string* error) {
    auto port = std::make_shared<OutputPort>(d.id, bridgeConfig(2, asClock ? 0 : (config_.exclusive ? config_.blockFrames : kSharedPeriod)));
    OutputCallback cb;
    if (asClock) {
        cb = [this, p = port.get()](float* out, int count, int channels, double time) {
            engine_.render(p, out, count, channels, time);
        };
    } else {
        auto scratch = std::make_shared<std::vector<float>>(8192 * 2);
        cb = [port, scratch](float* out, int count, int channels, double time) {
            float* buf = scratch->data();
            int done = 0;
            while (done < count) {
                const int n = std::min(count - done, 8192);
                port->bridge.pull(buf, static_cast<std::size_t>(n), time + done / kSampleRate);
                float* dst = out + static_cast<std::ptrdiff_t>(done) * channels;
                for (int i = 0; i < n; ++i) {
                    if (channels == 1) {
                        dst[i] = (buf[2 * i] + buf[2 * i + 1]) * 0.5f;
                        continue;
                    }
                    dst[i * channels] = buf[2 * i];
                    dst[i * channels + 1] = buf[2 * i + 1];
                    for (int c = 2; c < channels; ++c) dst[i * channels + c] = 0;
                }
                done += n;
            }
        };
    }
    auto stream = backend_.openOutput(d, config_, std::move(cb), error);
    if (!stream) return false;
    if (asClock) {
        // only one thread may render the engine: stop the previous clock before starting this one
        if (!clockKey_.empty()) closeStream(clockKey_);
        if (timer_) {
            timer_->stop();
            timer_.reset();
        }
    }
    if (!stream->start(error)) return false;
    port->streaming = !asClock;
    OpenStream s{std::move(stream), d, nullptr, port, asClock};
    {
        std::lock_guard lock(statsMutex_);
        statStreams_.push_back(s.stream.get());
        statOutputs_.push_back(port);
    }
    const std::string key = "out:" + d.id;
    streams_[key] = std::move(s);
    if (asClock) clockKey_ = key;
    return true;
}

void DeviceManager::reconcile() {
    DeviceDemand demand;
    bool refresh = false;
    {
        std::lock_guard lock(mutex_);
        demand = demand_;
        refresh = refresh_;
        refresh_ = false;
        demandChanged_ = false;
    }
    bool anyFailed = false;
    for (auto& [k, s] : streams_) anyFailed = anyFailed || s.stream->failed();
    if (refresh || anyFailed || devices_.empty()) devices_ = backend_.enumerate();

    std::vector<std::string> errors;
    const StreamConfig cfg{demand.mode == "exclusive", demand.blockFrames};
    if (cfg.exclusive != config_.exclusive || cfg.blockFrames != config_.blockFrames) {
        // every stream is opened for the mode and period: reopen them all
        std::vector<std::string> keys;
        for (auto& [k, s] : streams_) keys.push_back(k);
        for (auto& k : keys) closeStream(k);
        if (timer_) {
            timer_->stop();
            timer_.reset();
        }
        lastFailure_.clear();
        config_ = cfg;
    }

    auto device = [&](const std::string& id, bool input) -> const DeviceInfo* {
        for (const auto& d : devices_)
            if (d.id == id && d.input == input && d.connected) return &d;
        return nullptr;
    };
    auto contains = [](const std::vector<std::string>& v, const std::string& id) {
        return std::find(v.begin(), v.end(), id) != v.end();
    };

    // close what is no longer wanted, gone, or broken
    {
        std::vector<std::string> drop;
        for (auto& [k, s] : streams_) {
            const bool wanted = s.device.input ? contains(demand.inputs, s.device.id) : contains(demand.outputs, s.device.id);
            const bool present = device(s.device.id, s.device.input) != nullptr;
            if (!wanted || !present || s.stream->failed()) {
                if (s.stream->failed()) lastFailure_[k] = qpcSecondsNow();
                drop.push_back(k);
            }
        }
        for (auto& k : drop) closeStream(k);
    }

    // clock: the first usable output in preference order
    std::string clockId;
    for (const auto& id : demand.outputs) {
        const DeviceInfo* d = device(id, false);
        if (!d) continue;
        const std::string key = "out:" + id;
        if (key == clockKey_) {
            clockId = id;
            break;
        }
        if (!retryAllowed(key)) continue;
        // becoming the clock means reopening the stream in clock mode
        if (streams_.count(key)) closeStream(key);
        std::string err;
        if (openOutput(*d, true, &err)) {
            clockId = id;
            lastFailure_.erase(key);
            break;
        }
        lastFailure_[key] = qpcSecondsNow();
        errors.push_back(d->name + ": " + err);
    }
    if (clockId.empty()) {
        if (!clockKey_.empty()) closeStream(clockKey_);
        if (!timer_) {
            timer_ = makeTimerClock(config_.blockFrames, [this](float* out, int count, int channels, double time) {
                engine_.render(nullptr, out, count, channels, time);
            });
            std::string err;
            timer_->start(&err);
        }
    }

    // the other outputs and the inputs
    for (const auto& id : demand.outputs) {
        const std::string key = "out:" + id;
        const DeviceInfo* d = device(id, false);
        if (!d || streams_.count(key) || !retryAllowed(key)) continue;
        std::string err;
        if (openOutput(*d, false, &err)) {
            lastFailure_.erase(key);
        } else {
            lastFailure_[key] = qpcSecondsNow();
            errors.push_back(d->name + ": " + err);
        }
    }
    for (const auto& id : demand.inputs) {
        const std::string key = "in:" + id;
        const DeviceInfo* d = device(id, true);
        if (!d || streams_.count(key) || !retryAllowed(key)) continue;
        std::string err;
        if (openInput(*d, &err)) {
            lastFailure_.erase(key);
        } else {
            lastFailure_[key] = qpcSecondsNow();
            errors.push_back(d->name + ": " + err);
        }
    }

    State st;
    st.devices = devices_;
    for (auto& [k, s] : streams_) {
        if (s.input) st.ports.inputs[s.device.id] = s.input;
        if (s.output) st.ports.outputs[s.device.id] = s.output;
    }
    if (clockKey_.empty()) {
        st.clockActive = "timer";
        st.latencyMs = config_.blockFrames * 1000.0 / kSampleRate;
    } else {
        const auto& cs = streams_.at(clockKey_);
        st.clockActive = "device/" + cs.device.id;
        st.latencyMs = cs.stream->latencySeconds() * 1000.0;
    }
    st.errors = std::move(errors);
    if (listener_) listener_(st);
}

} // namespace dc::mixd
