// The video mixer (lane B1): every video mix of the engine's tree (/video/mix/#, up to 4), the sources they show
// (shared: a camera used by two mixes is opened once) and their scenes, layers and transitions.
//
// VideoEngine owns one D3D11 device and one video thread. Each running mix ticks on its own fixed-rate schedule
// on that thread (the design doc: the compositor keeps running even when sources stop): poll the sources it
// needs, draw the program scene (or the transition) into the mix's canvas, hand the canvas to the mix's
// consumers (the outputs: recording, virtual camera, full-screen, preview; lane B2+B3 and N).
//
// A mix runs while somebody holds a lease on it (acquire()). Sources that no running mix needs are closed.
// When the GPU is reset or removed, the engine recreates the device, the sources and the compositors and keeps
// ticking; the frame's generation number goes up so outputs know to rebuild their GPU objects. The generation
// also goes up when the mix's canvas size or frame rate changes.
#pragma once

#include "d3d.hpp"
#include "settings.hpp"

#include "dc/protocol/messages.hpp"
#include "dc/protocol/param_tree.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dc::capd {

struct MixFrame {
    ID3D11Texture2D* canvas = nullptr;  // BGRA, premultiplied alpha, width x height; valid during the call only
    std::int64_t tick = 0;              // counts from 0 when the mix started running
    std::int64_t qpc = 0;               // due time of the tick
    const Device* device = nullptr;     // the device that owns canvas
    std::uint64_t generation = 0;       // changes after a device reset or a canvas size / rate change
    int width = 0, height = 0, fps = 0;
};

class VideoEngine;

// A lease on one running mix. The mix keeps running until every lease is gone.
class VideoMix {
public:
    // Called on the video thread once per tick of the mix.
    using FrameConsumer = std::function<void(const MixFrame&)>;
    // Older form used by the P2 recorder.
    using Consumer = std::function<void(ID3D11Texture2D* canvas, std::int64_t tick, std::int64_t qpc)>;

    struct Stats {
        std::uint64_t ticks = 0;
        std::uint64_t captured = 0;     // new images from the mix's sources
        std::uint64_t lateTicks = 0;    // ticks skipped because the loop fell behind by a whole frame
        std::uint64_t recoveries = 0;   // source and device problems recovered from
        double tickMsAvg = 0;           // work per tick (CPU side), last second
        double tickMsMax = 0;
    };

    ~VideoMix();
    VideoMix(const VideoMix&) = delete;
    VideoMix& operator=(const VideoMix&) = delete;

    int mix() const { return mix_; }
    // The device and canvas settings at the time of the call (they change on a reset / settings change).
    const Device& device() const;
    const MixSettings& settings() const;
    std::uint64_t generation() const;
    std::string sourceDescription() const;

    int addFrameConsumer(FrameConsumer c);
    int addConsumer(Consumer c);
    void removeConsumer(int id);  // returns after any running call of that consumer has finished

    Stats stats() const;
    std::string error() const;  // last device-level problem (empty while healthy)

private:
    friend class VideoEngine;
    struct Shared;
    VideoMix(std::shared_ptr<Shared> shared, int mix);
    std::shared_ptr<Shared> shared_;
    int mix_;
    std::vector<int> consumerIds_;
};

class VideoEngine {
public:
    // capd -> engine calls (/video/source/report, /video/devices/report). Called on the engine's own threads;
    // the receiver must be thread-safe.
    using Reporter = std::function<void(const std::string& method, const proto::Args& args)>;

    VideoEngine();
    ~VideoEngine();
    VideoEngine(const VideoEngine&) = delete;
    VideoEngine& operator=(const VideoEngine&) = delete;

    // The engine's tree (any thread; cheap when nothing video-related changed).
    void update(const proto::ParamTree& tree);

    // Lease on mix n (1..4). Null when the mix does not exist in the last tree. With next, the lease is on the
    // mix's "next" view instead (its preview scene alone, nextSceneView()), which runs as a mix of its own.
    std::shared_ptr<VideoMix> acquire(int mix, bool next = false);

    void setReporter(Reporter r);
    // Send the monitor and camera lists again, and the window list when withWindows (event video.devices.refresh).
    void refreshDeviceLists(bool withWindows);
    // Report every source's state again (after reconnecting to the engine).
    void reportAll();

    std::string adapterName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Monitors attached to the desktop, as "capd list" shows them.
std::vector<MonitorInfo> listMonitors();

}  // namespace dc::capd
