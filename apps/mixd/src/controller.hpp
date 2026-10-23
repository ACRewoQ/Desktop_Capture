#pragma once

// The engine's control side, on the network thread: owns the parameter tree through the control
// server, turns every change into a new mix plan and device demand, publishes device lists, input
// states, load and dropout counters, sends meters, and implements the engine's methods.

#include "device_manager.hpp"
#include "engine.hpp"
#include "plan_builder.hpp"

#include "dc/control/server.hpp"
#include "dc/shmaudio/ring.hpp"

#include <asio.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

namespace dc::mixd {

class Controller {
public:
    struct Options {
        BuildOptions build;
        std::chrono::milliseconds meterInterval{33};
        std::chrono::milliseconds statsInterval{500};
        bool verbose = true;
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

private:
    void seed(const std::vector<DeviceInfo>& devices);
    void registerMethods();
    void scheduleRebuild();
    void rebuild();
    void applyDeviceState(const DeviceManager::State& state);
    void publishDevices(const std::vector<DeviceInfo>& devices);
    void scheduleMeters();
    void scheduleStats();
    std::vector<proto::Change> deviceChanges(const std::vector<DeviceInfo>& devices) const;

    proto::Result addTrack(control::CallContext& ctx);
    proto::Result removeTrack(control::CallContext& ctx);
    proto::Result moveTrack(control::CallContext& ctx);
    proto::Result startRecording(control::CallContext& ctx);
    proto::Result stopRecording(control::CallContext& ctx);
    proto::Result captureReport(control::CallContext& ctx);
    void scheduleRecordingClock();

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
    std::vector<std::string> lastErrors_;
    std::vector<std::string> lastNotes_;
    std::array<std::uint32_t, kMaxStrips> shmChannels_{};
    std::array<bool, kMaxStrips> shmDisconnected_{};
    std::uint32_t meterSeq_ = 0;
    std::uint64_t loggedGlitches_ = 0;
};

} // namespace dc::mixd
