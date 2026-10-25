// Virtual camera output: a video mix's canvas to the virtual camera, 30 fps (design doc: NV12, 1920x1080 or
// 1280x720, 30 fps, scaled on the GPU when the mix has another size).
//
// Windows 11 (Media Foundation camera): the GPU converter makes NV12 at the camera size, a staging copy brings it to
// the CPU and the sender writes it into the bridge, where the camera source only copies it.
// Windows 10 (DirectShow camera): the same with BGRA at 1920x1080, packed to BGR24 for the softcam sender.
//
// Runs on the video thread (a consumer of the mix). Copies are read back one tick later through a small ring of
// staging textures with D3D11_MAP_FLAG_DO_NOT_WAIT, so the video thread never waits for the GPU.
#pragma once

#include "d3d.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace dc::vcam {
class Sender;
}

namespace dc::capd {

class Converter;

class VcamOutput {
public:
    struct Stats {
        std::uint64_t frames = 0;   // frames handed to the camera
        std::uint64_t skipped = 0;  // 30 fps slots without a free staging texture (GPU behind)
        bool cameraOpen = false;    // a camera app is reading
        std::string state;          // "active", "waiting" (no app has the camera open), "not installed: ..."
    };

    // resolution: "1080p" or "720p" (the Windows 10 camera is always 1920x1080).
    explicit VcamOutput(const std::string& resolution);
    ~VcamOutput();
    VcamOutput(const VcamOutput&) = delete;
    VcamOutput& operator=(const VcamOutput&) = delete;

    // One mix tick. generation: the mix's device generation; a change rebuilds the GPU objects.
    void onFrame(ID3D11Texture2D* canvas, const Device& dev, std::uint64_t generation, std::int64_t qpc);

    // The camera goes to its "no signal" picture (the output was removed or the mix stopped).
    void detach();

    Stats stats() const;
    int width() const { return width_; }
    int height() const { return height_; }

    static constexpr int kFps = 30;

private:
    void rebuild(const Device& dev);
    void retire();

    int width_ = 1920, height_ = 1080;
    bool nv12_ = true;
    std::unique_ptr<vcam::Sender> sender_;

    Device dev_;
    std::uint64_t generation_ = ~0ull;
    std::unique_ptr<Converter> converter_;
    ComPtr<ID3D11Texture2D> target_;
    struct Pending {
        ComPtr<ID3D11Texture2D> staging;
        bool busy = false;
        std::int64_t qpc = 0;
    };
    std::array<Pending, 3> ring_;
    std::size_t next_ = 0;
    std::int64_t lastSlot_ = -1;
    std::int64_t nextStatusQpc_ = 0;

    std::atomic<std::uint64_t> frames_{0}, skipped_{0};
    mutable std::mutex statusMutex_;
    bool cameraOpen_ = false;
    std::string state_ = "waiting";
};

}  // namespace dc::capd
