#pragma once

// In-process devices for tests and for running mixd without sound hardware (mixd --fake-devices).
// Inputs produce a sine tone; outputs measure the peak level they receive. Each stream runs on its own
// thread with its own clock (optionally off by some ppm), like real devices.

#include "device.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dc::mixd {

class FakeBackend final : public DeviceBackend {
public:
    struct Device {
        DeviceInfo info;
        double toneHz = 440;    // inputs: tone on every channel
        float toneLevel = 0.25f; // inputs: peak amplitude
        double ppm = 0;         // clock offset against the system clock
        int periodFrames = 480; // shared mode period
        bool sharedOnly = false; // no 48 kHz exclusive format: exclusive requests fall back to shared
        int exclusivePeriod = 0; // > 0: in exclusive mode the device only runs with this period
    };

    // An application playing a stereo tone while it runs.
    struct Process {
        ProcessInfo info;
        double toneHz = 660;
        bool running = true;
        std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true); // this run
    };

    // Two inputs (mono mic, stereo line) and two outputs (speakers, headphones), like the mock server.
    static std::vector<Device> standardDevices();

    explicit FakeBackend(std::vector<Device> devices = standardDevices(), std::vector<Process> processes = {});
    ~FakeBackend() override;

    void setConnected(const std::string& id, bool connected);
    // Replaces a device's id (Windows re-registering it after a rate change), keeping its name.
    void changeId(const std::string& id, const std::string& newId);
    // Starts (with a new pid) or ends an application; its captures fail when it ends.
    void setProcessRunning(const std::string& exe, bool running, std::uint32_t pid = 0);
    // Peak received by an output device since the last call (any thread).
    float takeOutputPeak(const std::string& id);
    // Frames an output device has played so far.
    std::uint64_t outputFrames(const std::string& id);

    std::vector<DeviceInfo> enumerate() override;
    void setChangeCallback(std::function<void()> cb) override;
    std::unique_ptr<Stream> openInput(const DeviceInfo& device, const StreamConfig& cfg, InputCallback cb,
                                      std::string* error) override;
    std::unique_ptr<Stream> openOutput(const DeviceInfo& device, const StreamConfig& cfg, OutputCallback cb,
                                       std::string* error) override;
    std::vector<ProcessInfo> enumerateProcesses() override;
    ProcessInfo findProcess(const std::string& exe) override;
    std::unique_ptr<Stream> openProcess(const ProcessInfo& process, InputCallback cb, std::string* error) override;

    struct Meter {
        std::atomic<float> peak{0};
        std::atomic<std::uint64_t> frames{0};
    };

private:
    std::mutex mutex_;
    std::vector<Device> devices_;
    std::vector<Process> processes_;
    std::map<std::string, std::shared_ptr<Meter>> meters_;
    std::function<void()> cb_;
};

} // namespace dc::mixd
