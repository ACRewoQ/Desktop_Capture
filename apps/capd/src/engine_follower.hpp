// capd's normal mode: connected to the engine as a service client, it records when the engine says so and reports
// back (docs/spec/protocol.md 5.4.1). The engine owns the settings and the state; capd only follows
// /capture/state: "starting" -> record and report "recording", "stopping" -> finish and report "idle", a failure
// -> report "error". The connection is retried every second; losing it stops a running recording (the engine marks
// the recording as failed on its side).
#pragma once

#include "recorder.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace dc::control {
class ControlClient;
}

namespace dc::capd {

#ifdef _WIN32
class VideoMix;
#endif

class EngineFollower {
public:
    struct Options {
        std::string host = "127.0.0.1";
        std::uint16_t port = 0;  // 0 = the default control port
        std::string shmSuffix;   // tests
        bool withVideo = true;   // false: audio only (Linux, tests)
    };

    explicit EngineFollower(Options options);
    ~EngineFollower();

    // Runs until stop becomes true. Returns the process exit code.
    int run(const std::atomic<bool>& stop);

private:
    bool ensureConnected();
    void onState(const std::string& state);
    void startRecording();
    void stopRecording(bool report);
    void report(const std::string& state, const std::string& file, const std::string& error,
                std::int64_t durationMs);

    Options options_;
    std::unique_ptr<control::ControlClient> client_;
    std::int64_t nextConnectMs_ = 0;
    std::string lastState_;
#ifdef _WIN32
    std::unique_ptr<VideoMix> mix_;
#endif
    std::unique_ptr<Recorder> recorder_;
};

}  // namespace dc::capd
