#pragma once

// Device backend interface. The engine opens every device as its own stream: one output device runs
// the engine (the clock), every other input and output is connected through a ClockBridge. The
// Windows implementation is WASAPI (wasapi_backend.cpp); tests and Linux builds use FakeBackend.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dc::mixd {

struct DeviceInfo {
    std::string id;
    std::string name;
    bool input = false;
    int channels = 0;
    bool connected = false; // active; unplugged devices are listed with connected = false
    bool isDefault = false;
    bool virtualCable = false; // "CABLE Input" and friends: listed with api "vmic"
    bool operator==(const DeviceInfo&) const = default;
};

struct StreamConfig {
    bool exclusive = false;
    int blockFrames = 256; // exclusive mode period; shared mode uses the device period
};

// Called on the stream thread with interleaved 48 kHz float frames.
using InputCallback = std::function<void(const float* frames, int count, double time, bool discontinuity)>;
// Fills count frames of channels interleaved floats (out may be nullptr for the timer clock).
using OutputCallback = std::function<void(float* out, int count, int channels, double time)>;

class Stream {
public:
    virtual ~Stream() = default;
    virtual bool start(std::string* error) = 0;
    virtual void stop() = 0; // joins the stream thread; callbacks have returned when it returns
    virtual int channels() const = 0;
    virtual int periodFrames() const = 0;    // frames per device period at 48 kHz
    virtual double latencySeconds() const = 0; // output buffer latency (outputs)
    virtual bool failed() const = 0;          // device lost or stream error; the manager reopens it
    virtual std::uint64_t glitches() const = 0; // device-side dropouts seen by the stream
};

class DeviceBackend {
public:
    virtual ~DeviceBackend() = default;
    virtual std::vector<DeviceInfo> enumerate() = 0;
    // Called from any thread when the device list may have changed or a stream failed.
    virtual void setChangeCallback(std::function<void()> cb) = 0;
    virtual std::unique_ptr<Stream> openInput(const DeviceInfo& device, const StreamConfig& cfg, InputCallback cb,
                                              std::string* error) = 0;
    virtual std::unique_ptr<Stream> openOutput(const DeviceInfo& device, const StreamConfig& cfg, OutputCallback cb,
                                               std::string* error) = 0;
};

// Runs an OutputCallback (with out = nullptr) at 48 kHz from a timer thread: the engine's clock when
// no output device is available. blockFrames per call on average, catching up after late wake-ups.
std::unique_ptr<Stream> makeTimerClock(int blockFrames, OutputCallback cb);

#ifdef _WIN32
std::unique_ptr<DeviceBackend> makeWasapiBackend();
#endif

} // namespace dc::mixd
