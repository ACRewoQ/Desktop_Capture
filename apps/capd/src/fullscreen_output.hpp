// Full-screen output: a video mix shown on one of the PC's monitors (design doc: the "メインPCのモニター（全画面表示）"
// destination, e.g. a projector or a confidence monitor).
//
// A borderless top-most window covers the chosen monitor; a flip-model swap chain on the mix's device shows the
// canvas, scaled with the aspect ratio kept. Its window lives on its own thread (message pump); frames come from
// the video thread, which never blocks: the swap chain is waitable and a frame is skipped while the display has
// not taken the previous one.
//
// When the monitor goes away only this output stops (design doc, disconnect table); it comes back by itself when
// the monitor is attached again. The window does not take focus and hides the mouse cursor over it.
#pragma once

#include "d3d.hpp"

#include <dxgi1_3.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace dc::capd {

class Converter;

class FullscreenOutput {
public:
    // Finds the desktop rectangle (physical pixels) of a monitor by the id in /video/monitor/display.
    using MonitorLookup = std::function<std::optional<RECT>(const std::string& id)>;

    struct Stats {
        std::uint64_t frames = 0;   // presented
        std::uint64_t skipped = 0;  // the display had not taken the previous frame yet
        std::string state;          // "active", "monitor not found: <id>", "error: ..."
    };

    FullscreenOutput(std::string monitorId, MonitorLookup lookup);
    ~FullscreenOutput();
    FullscreenOutput(const FullscreenOutput&) = delete;
    FullscreenOutput& operator=(const FullscreenOutput&) = delete;

    // One mix tick (video thread). generation: the mix's device generation; a change rebuilds the GPU objects.
    void onFrame(ID3D11Texture2D* canvas, const Device& dev, std::uint64_t generation, std::int64_t qpc);

    Stats stats() const;
    const std::string& monitorId() const { return monitorId_; }

private:
    void windowThread();
    void place(const RECT& r);
    bool ensureSwapChain(const Device& dev, std::uint64_t generation);
    void releaseSwapChain();
    void setState(std::string s);

    std::string monitorId_;
    MonitorLookup lookup_;

    // window thread
    std::thread thread_;
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<bool> quit_{false};
    std::atomic<DWORD> threadId_{0};

    // geometry published by the window thread (0 x 0 = monitor absent, window hidden)
    std::atomic<int> width_{0}, height_{0};
    std::atomic<std::uint64_t> placement_{0};  // bumps when the window was moved or resized

    // video thread
    Device dev_;
    std::uint64_t generation_ = ~0ull;
    std::uint64_t placementSeen_ = ~0ull;
    ComPtr<IDXGISwapChain2> swapChain_;
    HANDLE waitable_ = nullptr;
    std::unique_ptr<Converter> converter_;
    UINT bufferW_ = 0, bufferH_ = 0;

    std::atomic<std::uint64_t> frames_{0}, skipped_{0};
    mutable std::mutex stateMutex_;
    std::string state_ = "starting";
};

// Default lookup: the id of /video/monitor/display (monitor device path or \\.\DISPLAY2, see findMonitor) or an index
// in "capd list"; "" = primary.
std::optional<RECT> findMonitorRect(const std::string& id);

}  // namespace dc::capd
