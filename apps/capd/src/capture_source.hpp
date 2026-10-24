// A capture source keeps the newest desktop image in a BGRA texture that it owns. The video mix polls it once per
// tick and draws the newest image (the last one again when nothing changed).
#pragma once

#include "d3d.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace dc::capd {

class CaptureSource {
public:
    virtual ~CaptureSource() = default;

    virtual const char* name() const = 0;

    // Takes any new frame into texture(). Returns true if the image changed since the last call.
    // Never blocks for long; returns false when nothing new arrived.
    virtual bool poll() = 0;

    // Newest image (BGRA). Null until the first frame arrived. May be recreated when the monitor size changes.
    virtual ID3D11Texture2D* texture() const = 0;
    virtual UINT width() const = 0;
    virtual UINT height() const = 0;

    // QPC time of the newest frame as reported by the OS (0 if unknown).
    virtual std::int64_t frameQpc() const = 0;

    // Count of OS-side problems recovered from (access lost, pool recreated, ...).
    virtual std::uint64_t recoveries() const = 0;

    // The captured monitor or window went away (WGC item closed). The owner drops the source and looks again.
    virtual bool closed() const { return false; }
};

// Windows.Graphics.Capture, monitor capture. Win10 1903+.
std::unique_ptr<CaptureSource> createWgcSource(const Device& dev, const MonitorInfo& monitor, bool cursor = true);

// Windows.Graphics.Capture, one window.
std::unique_ptr<CaptureSource> createWgcWindowSource(const Device& dev, HWND window, bool cursor = true);

// DXGI Desktop Duplication (the Win10 fallback). The cursor is not drawn (it comes as a separate shape).
std::unique_ptr<CaptureSource> createDdaSource(const Device& dev, const MonitorInfo& monitor);

}  // namespace dc::capd
