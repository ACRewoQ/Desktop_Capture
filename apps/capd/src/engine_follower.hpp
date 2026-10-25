// capd's normal mode: connected to the engine as a service client, it runs the video mixer from the engine's tree
// (/video/*, protocol.md 4.5), feeds the virtual camera and the full-screen output (/video/vcam, /video/monitor,
// reported with /video/output/report, 5.4.4) and records when the engine says so and reports back
// (docs/spec/protocol.md 5.4.1). After a GPU reset the recording goes on in a new file (report recording with
// reason "gpu_reset"). The engine owns the settings and the state; capd only follows
// /capture/state: "starting" -> record and report "recording", "stopping" -> finish and report "idle", a failure
// -> report "error". The connection is retried every second; losing it stops a running recording (the engine marks
// the recording as failed on its side).
#pragma once

#include "recorder.hpp"

#include "dc/protocol/messages.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dc::control {
class ControlClient;
}

namespace dc::capd {

#ifdef _WIN32
class VideoEngine;
class VideoMix;
class VcamOutput;
class FullscreenOutput;
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
                std::int64_t durationMs, const std::string& reason = {});
    void followVideo();
    void followOutputs();
    void reportOutput(const std::string& output, const std::string& state, const std::string& detail);
    bool openRecorder(const std::string& path);
    void checkSegment();

    Options options_;
    std::unique_ptr<control::ControlClient> client_;
    std::int64_t nextConnectMs_ = 0;
    std::string lastState_;
#ifdef _WIN32
    std::unique_ptr<VideoEngine> video_;
    std::shared_ptr<VideoMix> mix_;  // lease on the recorded mix while recording
    std::uint64_t videoRev_ = 0;
    // an output of one mix: the lease, the output and its consumer on the mix
    template <class Output>
    struct OutputHost {
        int mix = 0;
        std::string setting;  // resolution / display
        std::shared_ptr<VideoMix> lease;
        std::unique_ptr<Output> out;
        int consumer = 0;
        std::string failure;  // the output could not be created
    };
    OutputHost<VcamOutput> vcam_;
    OutputHost<FullscreenOutput> monitor_;
    const void* recordDevice_ = nullptr;  // the device the running recording encodes on
    bool segmentPending_ = false;         // a GPU reset closed the file; the next one starts on the new device
    std::int64_t segmentSinceMs_ = 0;
#endif
    RecordSettings recordSettings_;  // of the running recording (segments keep them)
    std::map<std::string, std::pair<std::string, std::string>> reportedOutputs_;
    std::mutex videoCallsMutex_;  // reports from the video engine's threads, sent by run()
    std::vector<std::pair<std::string, proto::Args>> videoCalls_;
    std::atomic<bool> refreshWindows_{false};
    std::unique_ptr<Recorder> recorder_;
};

}  // namespace dc::capd
