#pragma once

// The engine's control side, on the network thread: owns the parameter tree through the control
// server, turns every change into a new mix plan and device demand, publishes device lists, input
// states, load and dropout counters, sends meters, and implements the engine's methods.

#include "device_manager.hpp"
#include "engine.hpp"
#include "fx_manager.hpp"
#include "plan_builder.hpp"
#include "recorder.hpp"
#include "sampler.hpp"

#include "dc/control/server.hpp"
#include "dc/shmaudio/ring.hpp"

#include <asio.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dc::mixd {

class LanService;

class Controller {
public:
    struct Options {
        BuildOptions build;
        std::chrono::milliseconds meterInterval{33};
        std::chrono::milliseconds statsInterval{500};
        bool verbose = true;
        PluginHost* plugins = nullptr; // VST3 host, null when the build has none
        LanService* lan = nullptr; // LAN streams, inputs and pairing (optional)
    };

    Controller(asio::io_context& io, control::ControlServer& server, DeviceManager& devices, Engine& engine,
               shm::AudioRingWriter* shm, Options options);

    // Seeds the tree (defaults, the given devices, master to the default output) and starts timers.
    void start(const std::vector<DeviceInfo>& devices);
    void stop();

    // Hooks. onDelta runs on the network thread (ControlServer::Options::onDelta); onDeviceState may
    // be called from any thread.
    void onDelta(const proto::Delta& delta);
    void onDeviceState(const DeviceManager::State& state);
    // ControlServer::Options::onClient (network thread): follows the capture process (capd).
    void onClient(const control::ClientInfo& client, bool connected);
    // The LAN service added or removed ports (network thread).
    void portsChanged() { scheduleRebuild(); }

private:
    void seed(const std::vector<DeviceInfo>& devices);
    void registerMethods();
    void scheduleRebuild();
    void rebuild();
    void applyDeviceState(const DeviceManager::State& state);
    void publishDevices(const std::vector<DeviceInfo>& devices);
    void publishProcesses(const std::vector<ProcessInfo>& processes);
    void rematch();
    void scheduleMeters();
    void scheduleStats();
    std::vector<proto::Change> deviceChanges(const std::vector<DeviceInfo>& devices) const;

    proto::Result addTrack(control::CallContext& ctx);
    proto::Result removeTrack(control::CallContext& ctx);
    proto::Result moveTrack(control::CallContext& ctx);
    proto::Result startRecording(control::CallContext& ctx);
    proto::Result stopRecording(control::CallContext& ctx);
    proto::Result captureReport(control::CallContext& ctx);
    proto::Result outputReport(control::CallContext& ctx);
    void scheduleRecordingClock();

    // per-track recording (recorder.hpp)
    proto::Result startTake(bool fromCapture, std::string* folder);
    void stopTake(const std::string& error);
    void scheduleRecorderClock();
    void captureStateChanged(const std::string& state);

    // sampler (sampler.hpp)
    void padFileChanged(int pad, const std::string& file);
    void sampleLoaded(SampleLoader::Result r);
    std::shared_ptr<const SamplerTable> samplerTable() const;
    void publishPlaying();
    proto::Result samplerCommand(control::CallContext& ctx, Sampler::Command::Type type);

    asio::io_context& io_;
    control::ControlServer& server_;
    DeviceManager& devices_;
    Engine& engine_;
    shm::AudioRingWriter* shm_;
    Options options_;
    asio::steady_timer meterTimer_;
    asio::steady_timer statsTimer_;
    asio::steady_timer recordTimer_;
    std::chrono::steady_clock::time_point recordStart_;
    int captureClients_ = 0; // connected capd processes (role Service, name "capd")
    bool rebuildPending_ = false;
    bool stopped_ = false;
    std::uint64_t generation_ = 0;
    PortTable ports_;
    std::vector<DeviceInfo> deviceList_;
    std::vector<ProcessInfo> processList_;
    std::map<std::string, KnownDevice> known_; // every device seen since the start, for rematch()
    std::vector<std::string> lastErrors_;
    std::vector<std::string> lastNotes_;
    std::array<std::uint32_t, kMaxStrips> shmChannels_{};
    std::array<bool, kMaxStrips> shmDisconnected_{};
    std::uint32_t meterSeq_ = 0;
    std::uint64_t loggedGlitches_ = 0;

    std::shared_ptr<RecorderTake> take_;
    bool takeFromCapture_ = false;
    asio::steady_timer recorderTimer_;

    struct PadState {
        std::string file;
        std::uint64_t serial = 0;
        std::shared_ptr<const SampleData> sample;
    };
    std::array<PadState, kSamplerPadCount> pads_;
    std::uint64_t loadSerial_ = 0;
    std::array<std::uint64_t, 2> lastPlaying_{};
    std::unique_ptr<SampleLoader> loader_;
    FxManager fx_;
};

} // namespace dc::mixd
