// A video mixer source (/video/source/#): keeps its newest picture in a texture of the mixer's device. All
// device-context work happens in poll(), on the video thread; sources that read from a camera or a file do the
// blocking part on a thread of their own and hand over the newest frame.
//
// A source never throws out of poll() for problems of its target (monitor unplugged, camera busy, file missing):
// it reports them through status() and retries by itself. Exceptions out of poll() mean a device problem.
#pragma once

#include "d3d.hpp"
#include "video_config.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace dc::capd {

enum class AlphaMode { Opaque, Straight, Premultiplied };

struct SourceStatus {
    std::string state = "disconnected";  // ok | disconnected | error  (protocol.md 4.5)
    std::string error;
    int width = 0, height = 0;
    bool operator==(const SourceStatus&) const = default;
};

class VideoSource {
public:
    virtual ~VideoSource() = default;

    // Takes new frames (and retries a lost target now and then). Returns true when the picture changed.
    virtual bool poll() = 0;

    // Newest picture, null while there is none. The view stays valid until the next poll().
    virtual ID3D11ShaderResourceView* view() = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual AlphaMode alpha() const { return AlphaMode::Opaque; }

    virtual SourceStatus status() const = 0;
    virtual std::string description() const = 0;
    virtual std::uint64_t recoveries() const { return 0; }
    // QPC time the OS captured the newest picture at (monitor and window capture); 0 = unknown.
    virtual std::int64_t frameQpc() const { return 0; }
};

// Shared services for sources (Direct2D / DirectWrite for text, the "no signal" picture). One per device.
class TextRenderer;

struct SourceContext {
    const Device* device = nullptr;
    TextRenderer* text = nullptr;
};

// Creates the source for a config (never null; an unsupported kind gives a source that reports an error).
// Color sources have no texture: the compositor fills them; this returns a source that only reports "ok".
std::unique_ptr<VideoSource> createVideoSource(const SourceContext& ctx, const SourceConfig& cfg);

// Keeps one texture and its shader resource view; recreated when size or format changes.
class TextureSlot {
public:
    // Returns the texture, (re)creating it with the given size and format when needed.
    ID3D11Texture2D* ensure(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, UINT bind);
    ID3D11Texture2D* texture() const { return texture_.Get(); }
    ID3D11ShaderResourceView* view(ID3D11Device* device);
    void reset();

private:
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> view_;
    UINT width_ = 0, height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

// --- device lists (also used for the /video/devices report) ---

struct DisplayInfo {
    std::string id;       // monitor device path (stable across reboots), or the GDI name when unavailable
    std::string name;     // friendly monitor name
    std::string gdiName;  // \\.\DISPLAY1
    int width = 0, height = 0;
    bool primary = false;
    bool operator==(const DisplayInfo&) const = default;
};
std::vector<DisplayInfo> listDisplays();

// The monitor with this ID (DisplayInfo::id, or a GDI name); "" = the primary monitor. Null when not attached.
// Shared with the full-screen output (/video/monitor/display uses the same IDs).
std::unique_ptr<MonitorInfo> findMonitor(const std::string& id);

struct WindowInfo {
    HWND hwnd = nullptr;
    std::string exe;    // file name only, e.g. game.exe
    std::string title;
    bool operator==(const WindowInfo&) const = default;
};
std::vector<WindowInfo> listWindows();

struct CameraInfo {
    std::string id;    // Media Foundation symbolic link
    std::string name;
    bool operator==(const CameraInfo&) const = default;
};
std::vector<CameraInfo> listCameras();  // the calling thread must have COM initialised and MFStartup done

}  // namespace dc::capd
