// One video mix: its own fixed-rate timer (the design doc: the compositor keeps running even when sources stop),
// the capture source of its single layer, and the compositor. Each tick it takes the newest captured image, draws
// the scene into the canvas and hands the canvas to the outputs (P2: the recording; P3 adds preview, virtual
// camera and full-screen outputs as more consumers).
#pragma once

#include "capture_source.hpp"
#include "compositor.hpp"
#include "d3d.hpp"
#include "settings.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace dc::capd {

class VideoMix {
public:
    // Runs on the mix thread once per tick. tick counts from 0 at start(); qpc is the tick's due time.
    using Consumer = std::function<void(ID3D11Texture2D* canvas, std::int64_t tick, std::int64_t qpc)>;

    struct Stats {
        std::uint64_t ticks = 0;
        std::uint64_t captured = 0;     // new images from the capture source
        std::uint64_t lateTicks = 0;    // ticks skipped because the loop fell behind by a whole frame
        std::uint64_t recoveries = 0;   // capture source problems recovered from
        double tickMsAvg = 0;           // work per tick (CPU side), last second
        double tickMsMax = 0;
    };

    explicit VideoMix(const MixSettings& s);
    ~VideoMix();
    VideoMix(const VideoMix&) = delete;
    VideoMix& operator=(const VideoMix&) = delete;

    const Device& device() const { return dev_; }
    const MixSettings& settings() const { return settings_; }
    std::string sourceDescription() const;

    void start();
    void stop();

    int addConsumer(Consumer c);
    void removeConsumer(int id);  // returns after any running call of that consumer has finished

    Stats stats() const;
    std::string error() const;  // set when the loop stopped on a GPU error (device removed, ...)

private:
    void run();
    void tick(std::int64_t index, std::int64_t qpc);

    MixSettings settings_;
    MonitorInfo monitor_;
    Device dev_;
    std::unique_ptr<CaptureSource> source_;
    std::unique_ptr<Compositor> compositor_;

    std::mutex consumersMutex_;  // held while consumers run
    std::map<int, Consumer> consumers_;
    int nextId_ = 1;

    std::thread thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex statsMutex_;
    Stats stats_;
    std::string error_;
};

// Monitors attached to the desktop, as "capd list" shows them.
std::vector<MonitorInfo> listMonitors();
const MonitorInfo& pickMonitor(const std::vector<MonitorInfo>& monitors, int index);

}  // namespace dc::capd
