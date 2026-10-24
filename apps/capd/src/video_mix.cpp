#include "video_mix.hpp"

#include "dc/shmaudio/clock.hpp"

#include <algorithm>
#include <avrt.h>

namespace dc::capd {

std::vector<MonitorInfo> listMonitors() { return enumerateMonitors(); }

const MonitorInfo& pickMonitor(const std::vector<MonitorInfo>& monitors, int index) {
    if (monitors.empty()) throw std::runtime_error("no monitor attached to the desktop");
    if (index >= 0) {
        if (index >= static_cast<int>(monitors.size())) throw std::runtime_error("no such monitor index");
        return monitors[static_cast<size_t>(index)];
    }
    for (const auto& m : monitors)
        if (m.primary) return m;
    return monitors.front();
}

VideoMix::VideoMix(const MixSettings& s) : settings_(s) {
    const auto monitors = listMonitors();
    monitor_ = pickMonitor(monitors, s.monitor);
    // The device must live on the adapter that drives the captured monitor (hybrid-GPU laptops).
    dev_ = createDevice(monitor_.adapter.Get());
    source_ = s.source == "dda" ? createDdaSource(dev_, monitor_) : createWgcSource(dev_, monitor_);
    compositor_ = std::make_unique<Compositor>(dev_, static_cast<UINT>(s.width), static_cast<UINT>(s.height));
}

VideoMix::~VideoMix() { stop(); }

std::string VideoMix::sourceDescription() const {
    return std::string(source_->name()) + " " + monitor_.deviceName + " (" +
           std::to_string(monitor_.desktop.right - monitor_.desktop.left) + "x" +
           std::to_string(monitor_.desktop.bottom - monitor_.desktop.top) + ") on " + dev_.adapterName;
}

void VideoMix::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void VideoMix::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

int VideoMix::addConsumer(Consumer c) {
    std::lock_guard lock(consumersMutex_);
    const int id = nextId_++;
    consumers_[id] = std::move(c);
    return id;
}

void VideoMix::removeConsumer(int id) {
    std::lock_guard lock(consumersMutex_);
    consumers_.erase(id);
}

VideoMix::Stats VideoMix::stats() const {
    std::lock_guard lock(statsMutex_);
    return stats_;
}

std::string VideoMix::error() const {
    std::lock_guard lock(statsMutex_);
    return error_;
}

void VideoMix::tick(std::int64_t index, std::int64_t qpc) {
    if (source_->poll()) {
        std::lock_guard lock(statsMutex_);
        ++stats_.captured;
    }
    std::vector<LayerDraw> layers;
    if (source_->texture()) layers.push_back(compositor_->fit(source_->texture(), source_->width(), source_->height()));
    compositor_->render(layers);

    std::lock_guard lock(consumersMutex_);
    for (auto& [id, c] : consumers_) c(compositor_->canvas(), index, qpc);
}

void VideoMix::run() {
    // MMCSS "Capture" class: keeps the tick on time when the machine is busy (as OBS does for its video thread).
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    const std::int64_t freq = shm::qpcFrequency();
    const std::int64_t fps = std::max(1, settings_.fps);
    const std::int64_t t0 = shm::nowQpc();
    std::int64_t index = 0;
    std::int64_t secondEnd = t0 + freq;
    double secSum = 0, secMax = 0;
    std::uint64_t secTicks = 0;

    while (running_) {
        // Due time of tick `index`, exact in QPC units (no accumulated rounding).
        const std::int64_t due = t0 + index * freq / fps;
        std::int64_t now = shm::nowQpc();
        if (due > now) {
            LARGE_INTEGER rel;
            rel.QuadPart = -static_cast<LONGLONG>((due - now) * 10'000'000 / freq);
            SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 1000);
            now = shm::nowQpc();
        } else if ((now - due) * fps >= freq) {
            // Fell behind by at least a frame: skip the missed ticks (they become gaps the recording fills).
            const std::int64_t missed = (now - due) * fps / freq;
            index += missed;
            std::lock_guard lock(statsMutex_);
            stats_.lateTicks += static_cast<std::uint64_t>(missed);
            continue;
        }

        const std::int64_t w0 = shm::nowQpc();
        try {
            tick(index, t0 + index * freq / fps);
        } catch (const std::exception& e) {
            // Most likely the GPU was reset or removed. P3 recreates the device and continues; P2 stops here.
            std::lock_guard lock(statsMutex_);
            error_ = std::string("video mix stopped: ") + e.what();
            running_ = false;
            break;
        }
        const double ms = double(shm::nowQpc() - w0) * 1000.0 / double(freq);
        secSum += ms;
        secMax = std::max(secMax, ms);
        ++secTicks;
        ++index;

        if (shm::nowQpc() >= secondEnd) {
            secondEnd += freq;
            std::lock_guard lock(statsMutex_);
            stats_.ticks = static_cast<std::uint64_t>(index);
            stats_.recoveries = source_->recoveries();
            stats_.tickMsAvg = secTicks ? secSum / double(secTicks) : 0;
            stats_.tickMsMax = secMax;
            secSum = secMax = 0;
            secTicks = 0;
        }
    }
    if (timer) CloseHandle(timer);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace dc::capd
