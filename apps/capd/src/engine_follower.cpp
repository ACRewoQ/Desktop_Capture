#include "engine_follower.hpp"

#include "settings.hpp"

#include "dc/control/client.hpp"
#include "dc/protocol/constants.hpp"

#ifdef _WIN32
#include "video_mix.hpp"
#endif

#include <chrono>
#include <ctime>
#include <iostream>
#include <thread>

namespace dc::capd {

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

EngineFollower::EngineFollower(Options options) : options_(std::move(options)) {
    if (options_.port == 0) options_.port = proto::kDefaultControlPort;
}

EngineFollower::~EngineFollower() { stopRecording(false); }

bool EngineFollower::ensureConnected() {
    if (client_ && client_->connected()) return true;
    if (client_) {
        std::cout << "engine connection lost\n" << std::flush;
        stopRecording(false);  // the engine marks the recording as failed when capd disconnects
        client_.reset();
        lastState_.clear();
    }
    if (nowMs() < nextConnectMs_) return false;
    nextConnectMs_ = nowMs() + 1000;

    control::ControlClient::Options o;
    o.host = options_.host;
    o.port = options_.port;
    o.clientName = "capd";
    o.role = proto::ClientRole::Service;
    auto client = std::make_unique<control::ControlClient>(o);
    std::string err;
    if (!client->connect(std::chrono::milliseconds(2000), &err)) return false;
    std::cout << "connected to the engine at " << options_.host << ":" << options_.port << "\n" << std::flush;
    client_ = std::move(client);
    return true;
}

int EngineFollower::run(const std::atomic<bool>& stop) {
    std::cout << "capd: following the engine at " << options_.host << ":" << options_.port
              << " (Ctrl+C to exit)\n"
              << std::flush;
    while (!stop) {
        if (ensureConnected()) {
            const auto state = client_->tree().getAs<std::string>("/capture/state").value_or("idle");
            if (state != lastState_) {
                lastState_ = state;
                onState(state);
            }
            if (recorder_) {
                const auto st = recorder_->status();
                if (!st.error.empty()) {
                    std::cout << "recording failed: " << st.error << "\n" << std::flush;
                    stopRecording(false);
                    report("error", st.file, st.error, st.elapsedMs);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    stopRecording(false);
    return 0;
}

void EngineFollower::onState(const std::string& state) {
    if (state == "starting") {
        if (recorder_) stopRecording(false);  // a stale one (should not happen)
        startRecording();
    } else if (state == "stopping") {
        stopRecording(true);
    } else if (state == "idle" || state == "error") {
        // The engine gave up on this recording (or was reset): stop without a report.
        stopRecording(false);
    }
}

void EngineFollower::startRecording() {
    std::string err;
    const auto settings = settingsFromTree(client_->tree(), &err);
    if (!settings) {
        report("error", "", err, 0);
        return;
    }
    RecordSettings s = *settings;
#ifndef _WIN32
    s.withVideo = false;
#endif
    if (!options_.withVideo) s.withVideo = false;
    const std::string path = recordingPath(s, std::time(nullptr));
    try {
#ifdef _WIN32
        if (s.withVideo) {
            mix_ = std::make_unique<VideoMix>(s.mix);
            mix_->start();
        }
        recorder_ = std::make_unique<Recorder>(s, mix_.get(), path, options_.shmSuffix);
#else
        recorder_ = std::make_unique<Recorder>(s, path, options_.shmSuffix);
#endif
        recorder_->start();
    } catch (const std::exception& e) {
        recorder_.reset();
#ifdef _WIN32
        mix_.reset();
#endif
        std::cout << "cannot start recording: " << e.what() << "\n" << std::flush;
        report("error", path, e.what(), 0);
        return;
    }
    for (const auto& line : recorder_->log()) std::cout << line << "\n";
    std::cout << "recording " << path << "\n" << std::flush;
    report("recording", path, "", 0);
}

void EngineFollower::stopRecording(bool doReport) {
    if (!recorder_) {
        if (doReport) report("idle", "", "", 0);
        return;
    }
    const auto fin = recorder_->stop();
    recorder_.reset();
#ifdef _WIN32
    if (mix_) mix_->stop();
    mix_.reset();
#endif
    std::cout << "stopped " << fin.file << " (" << fin.elapsedMs << " ms, " << fin.videoFrames << " frames, "
              << fin.videoDropped << " dropped)" << (fin.error.empty() ? "" : " error: " + fin.error) << "\n"
              << std::flush;
    if (!doReport) return;
    if (fin.error.empty()) report("idle", fin.file, "", fin.elapsedMs);
    else report("error", fin.file, fin.error, fin.elapsedMs);
}

void EngineFollower::report(const std::string& state, const std::string& file, const std::string& error,
                            std::int64_t durationMs) {
    if (!client_ || !client_->connected()) return;
    proto::Args args{{"state", state}};
    if (!file.empty()) args["file"] = file;
    if (!error.empty()) args["error"] = error;
    if (state == "idle") args["duration_ms"] = durationMs;
    auto result = client_->call("/capture/report", std::move(args));
    if (result.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        std::cout << "engine did not answer /capture/report\n" << std::flush;
        return;
    }
    const auto r = result.get();
    if (r.code != proto::ErrorCode::Ok) std::cout << "engine refused /capture/report: " << r.message << "\n" << std::flush;
}

}  // namespace dc::capd
