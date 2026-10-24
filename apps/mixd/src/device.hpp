#pragma once

// Device backend interface. The engine opens every device as its own stream: one output device runs
// the engine (the clock), every other input and output is connected through a ClockBridge. The
// Windows implementation is WASAPI plus ASIO (wasapi_backend.cpp, asio_backend.cpp); tests and Linux
// builds use FakeBackend. Streams always speak 48 kHz: devices at other rates are converted inside
// the stream (clocksync::RateConverter).

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
    bool asio = false;         // an ASIO driver (input: all channels; output: one stereo pair)
    int sampleRate = 0;        // the device's own rate, 0 = unknown
    bool operator==(const DeviceInfo&) const = default;
};

// An application whose audio can be captured (WASAPI process loopback of its process tree).
struct ProcessInfo {
    std::string exe;       // executable file name as Windows reports it ("chrome.exe")
    std::uint32_t pid = 0; // root of the process tree with that name
    std::string title;     // a window title of the root process, "" if none
    bool operator==(const ProcessInfo&) const = default;
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
    // Exclusive mode was asked for but the device has no 48 kHz exclusive format: it runs shared.
    virtual bool sharedFallback() const { return false; }
    // The rate the device runs at; not 48000 means the stream converts.
    virtual int deviceRate() const { return 48000; }
    // Exclusive streams whose device signals less often than the period asked for (the Steinberg
    // UR22 driver wakes every 10 ms whatever the period): the frames per wake-up it really uses. The
    // stream reports failed() and the manager reopens it with this period.
    virtual int suggestedPeriod() const { return 0; }
    // Extra detail for the xrun log (inputs: what the last dropout looked like). Any thread.
    virtual std::string diagnostics() const { return {}; }
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

    // Applications with an audio session, one entry per executable name (the root process).
    virtual std::vector<ProcessInfo> enumerateProcesses() { return {}; }
    // The running process tree for an executable name (case-insensitive): its root, preferring a
    // tree with an audio session, then the oldest. pid 0 = not running.
    virtual ProcessInfo findProcess(const std::string& exe) { return {exe, 0, {}}; }
    // Captures what the process tree plays (48 kHz stereo). The stream reports failed() once the
    // process has exited.
    virtual std::unique_ptr<Stream> openProcess(const ProcessInfo& process, InputCallback cb, std::string* error) {
        if (error) *error = "process audio is not supported here";
        (void)process;
        (void)cb;
        return nullptr;
    }
};

// Runs an OutputCallback (with out = nullptr) at 48 kHz from a timer thread: the engine's clock when
// no output device is available. blockFrames per call on average, catching up after late wake-ups.
std::unique_ptr<Stream> makeTimerClock(int blockFrames, OutputCallback cb);

#ifdef _WIN32
// asio: also list and open the ASIO drivers (mixd --no-asio turns them off).
std::unique_ptr<DeviceBackend> makeWasapiBackend(bool asio = true);
#endif

} // namespace dc::mixd
