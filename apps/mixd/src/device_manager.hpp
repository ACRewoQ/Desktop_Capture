#pragma once

// Opens and closes device streams to match what the settings use (DeviceDemand), picks the clock
// device, and recovers from devices that disappear or fail. Runs on its own thread because opening a
// WASAPI stream can take a while; the control thread only hands it the latest demand.
//
// Clock rules (design doc "デバイス切断時の動作（音声）"): /system/clock_device if set and usable,
// otherwise the first usable master output, otherwise any output in use, otherwise the internal timer.
//
// Process inputs: one capture per application (executable name), whichever tracks use it. When the
// application exits the capture fails and is closed; the manager looks for the name again every few
// seconds and reconnects when the application runs again.

#include "device.hpp"
#include "engine.hpp"
#include "plan_builder.hpp"

#include <condition_variable>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dc::mixd {

class DeviceManager {
public:
    struct Options {
        double bridgeMinMs = 0; // lower bound of the bridge latency target (ms), --bridge-ms
    };

    // A device or application that the settings use went away or came back (device.lost /
    // device.restored).
    struct Event {
        std::string name;  // "device.lost" / "device.restored"
        std::string id;    // device id, or the executable name for processes
        std::string label; // device name / executable name
        std::string kind;  // input / output / process
        bool operator==(const Event&) const = default;
    };

    struct State {
        std::vector<DeviceInfo> devices;
        std::vector<ProcessInfo> processes; // applications with audio, for /devices/process
        PortTable ports;
        std::string clockActive; // "device/<id>" or "timer"
        double latencyMs = 0;
        std::vector<std::string> errors; // devices that could not be opened this round, with the reason
        std::vector<std::string> notes;  // open streams running differently from what was asked
        std::vector<Event> events;       // what changed since the previous State (each event once)
    };

    DeviceManager(DeviceBackend& backend, Engine& engine, Options options);
    ~DeviceManager();

    // listener is called on the manager thread after every change.
    void start(std::function<void(const State&)> listener);
    void stop();

    void setDemand(const DeviceDemand& demand); // any thread
    void refresh();                             // any thread: re-enumerate devices

    // Dropouts so far: device-side glitches plus bridge underruns/overruns. Any thread.
    std::uint64_t glitches() const;
    // The same counters per stream, for the log: "<device> in: device 0, bridge underruns 2 ...".
    std::vector<std::string> glitchDetails() const;

    // Enumerates synchronously (startup, before the manager thread runs).
    std::vector<DeviceInfo> enumerateNow() { return backend_.enumerate(); }

private:
    struct OpenStream {
        std::unique_ptr<Stream> stream;
        DeviceInfo device;
        std::shared_ptr<InputPort> input;
        std::shared_ptr<OutputPort> output;
        bool clock = false;
        bool process = false; // device.name is the executable name (lower case)
    };

    void run();
    void reconcile();
    void closeStream(const std::string& key);
    bool openInput(const DeviceInfo& d, std::string* error);
    bool openOutput(const DeviceInfo& d, bool asClock, std::string* error);
    bool openProcess(const ProcessInfo& p, std::string* error);
    void reconcileProcesses(const DeviceDemand& demand, std::vector<std::string>& errors);
    void trackAvailability(const DeviceDemand& demand, std::vector<Event>& events);
    clocksync::ClockBridge::Config bridgeConfig(int channels, int devicePeriod, bool virtualDevice = false) const;
    int clockPeriod() const;
    StreamConfig configFor(const std::string& key) const;
    bool retryAllowed(const std::string& key);
    void accountClosed(const OpenStream& s);

    DeviceBackend& backend_;
    Engine& engine_;
    Options options_;
    std::function<void(const State&)> listener_;

    std::mutex mutex_;
    std::condition_variable cv_;
    DeviceDemand demand_;
    bool demandChanged_ = true;
    bool refresh_ = true;
    bool wake_ = false;
    bool stop_ = false;
    std::thread thread_;

    // manager thread only
    std::vector<DeviceInfo> devices_;
    std::vector<ProcessInfo> processes_;
    double processScan_ = -1e9; // time of the last process list scan
    std::map<std::string, OpenStream> streams_; // key "in:<id>" / "out:<id>" / "proc:<exe>"
    std::map<std::string, bool> available_;     // demanded keys -> present at the last reconcile
    std::map<std::string, std::string> names_;  // device id -> name, every device seen so far
    std::unique_ptr<Stream> timer_;
    std::string clockKey_; // key of the clock stream, "" = timer
    StreamConfig config_;
    std::map<std::string, double> lastFailure_; // key -> time of the last failed open
    // exclusive devices that ignored the period asked for: first reopened with the period they use,
    // then, if that fails too, shared. Cleared when the mode or block size changes.
    std::map<std::string, int> periodOverride_;
    std::set<std::string> sharedOverride_;

    mutable std::mutex statsMutex_;
    std::uint64_t closedGlitches_ = 0; // from streams and bridges that were closed
    std::vector<std::shared_ptr<InputPort>> statInputs_;
    std::vector<std::shared_ptr<OutputPort>> statOutputs_;
    struct StatStream {
        Stream* stream;
        std::string label; // "<device name> in|out"
    };
    std::vector<StatStream> statStreams_;
};

} // namespace dc::mixd
