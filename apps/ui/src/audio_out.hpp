#pragma once

#include <QString>

#include <cstddef>
#include <functional>
#include <memory>

namespace dc::ui {

// A stereo 48 kHz float output for the monitor audio. The render callback runs on the output's own
// thread and fills `frames` interleaved frames; `now` is lan::nowSeconds() at the call (the clock the
// LAN receiver measures arrivals with).
//
//   Windows  WASAPI shared mode on the default output device, converted by the OS to the device's
//            format; follows a change of the default device and comes back after a device is lost.
//   null     a thread that renders 10 ms blocks into nothing (tests, other platforms).
class AudioOut {
public:
    using Render = std::function<void(float* out, std::size_t frames, double now)>;

    static std::unique_ptr<AudioOut> create(Render render, bool null = false);
    virtual ~AudioOut() = default;

    // Thread safe.
    virtual QString deviceName() const = 0;
    virtual QString error() const = 0;  // "" while it plays
};

} // namespace dc::ui
