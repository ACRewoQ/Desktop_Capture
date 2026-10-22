#pragma once

#include "dc/control/server.hpp"

#include <asio.hpp>

#include <chrono>
#include <cstdint>

namespace dc::mock {

// A fake engine that speaks the real protocol. It owns no audio: it seeds a plausible parameter
// tree (4 tracks, fake devices), implements the P0 methods (track add/remove/move, record
// start/stop) and sends fake meter levels. Lane C develops the UI against it until mixd is ready.
class MockEngine {
public:
    struct Options {
        bool meters = true;
        std::chrono::milliseconds meterInterval{33};
    };

    MockEngine(asio::io_context& io, control::ControlServer& server, Options options);

    // Seeds the tree and starts timers. Call once before running the io_context.
    void start();
    void stop();

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

    asio::io_context& io_;
    control::ControlServer& server_;
    Options options_;
    asio::steady_timer meterTimer_;
    asio::steady_timer recordTimer_;
    std::chrono::steady_clock::time_point recordStart_;
    std::uint32_t meterSeq_ = 0;
    double phase_ = 0.0;
    bool stopped_ = false;
};

} // namespace dc::mock
