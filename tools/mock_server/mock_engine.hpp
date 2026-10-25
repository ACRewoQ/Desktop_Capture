#pragma once

#include "fx_manager.hpp"
#include "mock_plugins.hpp"

#include "dc/control/server.hpp"

#include <asio.hpp>

#include <chrono>
#include <cstdint>

namespace dc::mock {

// A fake engine that speaks the real protocol. It owns no audio: it seeds a plausible parameter
// tree (4 tracks, fake devices), implements the P0 methods (track add/remove/move, record
// start/stop), the effect racks and plugin list of mixd (the same FxManager, with pretend VST3
// plugins) and sends fake meter levels. Lane C develops the UI against it until mixd is ready.
class MockEngine {
public:
    struct Options {
        bool meters = true;
        std::chrono::milliseconds meterInterval{33};
        // Leave recording to a connected capd (protocol.md 5.4.1: starting/stopping + /capture/report)
        // instead of pretending to record at once.
        bool delegateCapture = false;
    };

    MockEngine(asio::io_context& io, control::ControlServer& server, Options options);

    // Seeds the tree and starts timers. Call once before running the io_context.
    void start();
    void stop();
    // ControlServer::Options::onDelta: effect parameter / bypass / editor changes.
    void onDelta(const proto::Delta& delta) { fx_.onDelta(delta); }

private:
    void seed();
    void registerMethods();
    void scheduleMeters();
    void scheduleRecordingClock();
    std::vector<int> tracksInOrder() const;

    proto::Result addTrack(control::CallContext& ctx);
    proto::Result removeTrack(control::CallContext& ctx);
    proto::Result moveTrack(control::CallContext& ctx);
    proto::Result startRecording(control::CallContext& ctx);
    proto::Result stopRecording(control::CallContext& ctx);
    proto::Result captureReport(control::CallContext& ctx);

    asio::io_context& io_;
    control::ControlServer& server_;
    Options options_;
    asio::steady_timer meterTimer_;
    asio::steady_timer recordTimer_;
    std::chrono::steady_clock::time_point recordStart_;
    std::uint32_t meterSeq_ = 0;
    double phase_ = 0.0;
    bool stopped_ = false;
    MockPluginHost plugins_;
    mixd::FxManager fx_;
};

} // namespace dc::mock
