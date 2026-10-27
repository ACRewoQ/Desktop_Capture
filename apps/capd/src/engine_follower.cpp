#include "engine_follower.hpp"

#include "settings.hpp"

#include "dc/control/client.hpp"
#include "dc/protocol/constants.hpp"

#ifdef _WIN32
#include "fullscreen_output.hpp"
#include "lan_video_output.hpp"
#include "vcam_output.hpp"
#include "video_mix.hpp"

#include "dc/lanmedia/video_link.hpp"
#endif

#include <chrono>
#include <filesystem>
#include <ctime>
#include <iostream>
#include <thread>

namespace dc::capd {

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// A file name that is not taken yet: "name.mkv", else "name_2.mkv", "name_3.mkv", ...
bool exists(const std::string& utf8) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(std::u8string(utf8.begin(), utf8.end())), ec);
}

std::string unusedPath(const std::string& path) {
    if (!exists(path)) return path;
    const auto dot = path.find_last_of('.');
    const std::string stem = path.substr(0, dot), ext = dot == std::string::npos ? "" : path.substr(dot);
    for (int n = 2;; ++n) {
        std::string p = stem + "_" + std::to_string(n) + ext;
        if (!exists(p)) return p;
    }
}

#ifdef _WIN32
// The protocol's output states (5.4.4) from what the outputs say about themselves.
std::pair<std::string, std::string> vcamState(const VcamOutput::Stats& st) {
    if (st.state == "active" || st.state == "waiting") return {st.state, ""};
    return {"unavailable", st.state};
}
std::pair<std::string, std::string> monitorState(const FullscreenOutput::Stats& st) {
    if (st.state == "active" || st.state == "starting") return {"active", ""};
    if (st.state.rfind("monitor not found", 0) == 0) return {"no_display", st.state};
    return {"error", st.state.rfind("error: ", 0) == 0 ? st.state.substr(7) : st.state};
}
#endif

}  // namespace

EngineFollower::EngineFollower(Options options) : options_(std::move(options)) {
    if (options_.port == 0) options_.port = proto::kDefaultControlPort;
#ifdef _WIN32
    preview_.output = "preview";
    preview_.port = lan::kDefaultVideoPort;
    next_.output = "next";
    next_.port = lan::kDefaultNextVideoPort;
    lanOut_.output = "lan";
    lanOut_.port = static_cast<std::uint16_t>(lan::kDefaultVideoPort + 1);
#endif
}

EngineFollower::~EngineFollower() {
    stopRecording(false);
#ifdef _WIN32
    auto drop = [](auto& h) {
        if (h.lease && h.consumer) h.lease->removeConsumer(h.consumer);
        h = {};
    };
    if (vcam_.out) vcam_.out->detach();
    drop(vcam_);
    drop(monitor_);
    dropLanStreams();
    video_.reset();
#endif
}

bool EngineFollower::ensureConnected() {
    if (client_ && client_->connected()) return true;
    if (client_) {
        std::cout << "engine connection lost\n" << std::flush;
        stopRecording(false);  // the engine marks the recording as failed when capd disconnects
        dropLanStreams();      // the engine drops the subscriptions too
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
    client->onEvent = [this](const proto::Event& ev) {
        if (ev.name == "video.devices.refresh") refreshWindows_ = true;
    };
    std::string err;
    if (!client->connect(std::chrono::milliseconds(2000), &err)) return false;
    std::cout << "connected to the engine at " << options_.host << ":" << options_.port << "\n" << std::flush;
    client_ = std::move(client);
    reportedOutputs_.clear();  // send the output states again
#ifdef _WIN32
    videoRev_ = 0;
    if (video_) video_->reportAll();  // a restarted engine knows nothing about our sources
#endif
    return true;
}

void EngineFollower::followVideo() {
#ifdef _WIN32
    if (!options_.withVideo) return;
    if (!video_) {
        try {
            video_ = std::make_unique<VideoEngine>();
            video_->setReporter([this](const std::string& method, const proto::Args& args) {
                std::lock_guard lock(videoCallsMutex_);
                videoCalls_.emplace_back(method, args);
            });
            std::cout << "video mixer on " << video_->adapterName() << "\n" << std::flush;
        } catch (const std::exception& e) {
            std::cout << "video mixer unavailable: " << e.what() << "\n" << std::flush;
            options_.withVideo = false;
            return;
        }
    }
    const auto tree = client_->tree();
    if (tree.rev() != videoRev_) {
        videoRev_ = tree.rev();
        video_->update(tree);
    }
    if (refreshWindows_.exchange(false)) video_->refreshDeviceLists(true);
    followOutputs();
    followLanStreams();
#endif
    std::vector<std::pair<std::string, proto::Args>> calls;
    {
        std::lock_guard lock(videoCallsMutex_);
        calls.swap(videoCalls_);
    }
    for (auto& [method, args] : calls) client_->call(method, std::move(args));  // answers are not needed
}

void EngineFollower::followOutputs() {
#ifdef _WIN32
    const auto tree = client_->tree();
    auto follow = [&](auto& h, const char* mixPath, const char* settingPath, auto make) {
        const int want = static_cast<int>(tree.getAs<std::int64_t>(mixPath).value_or(0));
        const std::string setting = tree.getAs<std::string>(settingPath).value_or("");
        if (want != h.mix || setting != h.setting) {
            if (h.lease && h.consumer) h.lease->removeConsumer(h.consumer);
            h.consumer = 0;
            h.out.reset();  // the virtual camera goes to its "no signal" picture
            h.lease.reset();
            h.failure.clear();
            h.mix = want;
            h.setting = setting;
        }
        if (h.mix <= 0 || h.lease || !h.failure.empty()) return;
        h.lease = video_->acquire(h.mix);  // null while the mix does not exist: tried again on the next pass
        if (!h.lease) return;
        try {
            h.out = make(setting);
        } catch (const std::exception& e) {
            h.failure = e.what();
            h.lease.reset();
            return;
        }
        auto* out = h.out.get();
        h.consumer = h.lease->addFrameConsumer(
            [out](const MixFrame& f) { out->onFrame(f.canvas, *f.device, f.generation, f.qpc); });
    };
    follow(vcam_, "/video/vcam/mix", "/video/vcam/resolution",
           [](const std::string& res) { return std::make_unique<VcamOutput>(res); });
    follow(monitor_, "/video/monitor/mix", "/video/monitor/display",
           [](const std::string& display) { return std::make_unique<FullscreenOutput>(display, nullptr); });

    if (!vcam_.failure.empty()) reportOutput("vcam", "unavailable", vcam_.failure);
    else if (vcam_.out) {
        const auto [state, detail] = vcamState(vcam_.out->stats());
        reportOutput("vcam", state, detail);
    } else reportOutput("vcam", "off", "");
    if (!monitor_.failure.empty()) reportOutput("monitor", "error", monitor_.failure);
    else if (monitor_.out) {
        const auto [state, detail] = monitorState(monitor_.out->stats());
        reportOutput("monitor", state, detail);
    } else reportOutput("monitor", "off", "");
#endif
}

void EngineFollower::followLanStreams() {
#ifdef _WIN32
    const auto tree = client_->tree();
    followLan(preview_, tree);
    followLan(next_, tree);
    followLan(lanOut_, tree);
#endif
}

void EngineFollower::dropLanStreams() {
#ifdef _WIN32
    dropLan(preview_);
    dropLan(next_);
    dropLan(lanOut_);
#endif
}

#ifdef _WIN32
// The encoder runs only while somebody watches: the lease (and with it the mix, when no other output uses it) is
// taken when the first viewer subscribes and given back when the last one leaves.
void EngineFollower::followLan(LanHost& h, const proto::ParamTree& tree) {
    const LanStreamConfig config = lanStreamConfig(tree, h.output);
    auto to = lanDestinations(tree, config.stream);
    const std::size_t viewers = to.size();
    const bool want = config.mix > 0 && viewers > 0;
    if (want && !h.socket && nowMs() >= h.retryMs) {
        lan::VideoOutput::Options o;
        o.port = h.port;
        try {
            h.socket = std::make_unique<lan::VideoOutput>(o);
        } catch (const std::exception& e) {
            try {  // the port is taken (another sender on this PC): any free port, viewers answer to it
                o.port = 0;
                h.socket = std::make_unique<lan::VideoOutput>(o);
            } catch (const std::exception&) {
                std::cout << h.output << ": cannot open a UDP socket: " << e.what() << "\n" << std::flush;
                h.retryMs = nowMs() + 5000;
            }
        }
        if (h.socket && h.socket->localPort() != h.port)
            std::cout << h.output << ": UDP " << h.port << " is taken, sending from " << h.socket->localPort() << "\n"
                      << std::flush;
    }
    if (h.socket) h.socket->setDestinations(std::move(to));
    if (viewers != h.viewers) {
        std::cout << h.output << ": " << viewers << " viewer(s)\n" << std::flush;
        h.viewers = viewers;
    }

    const bool stale = h.out && h.out->stale();
    if (stale) {
        const std::string err = h.out->error();
        std::cout << h.output << ": " << (err.empty() ? "the GPU was reset, starting a new stream" : "encoder failed: " + err)
                  << "\n"
                  << std::flush;
        h.retryMs = nowMs() + (err.empty() ? 500 : 5000);
    }
    if (!want || config != h.config || stale) {
        if (h.lease && h.consumer) h.lease->removeConsumer(h.consumer);
        h.consumer = 0;
        h.out.reset();
        h.lease.reset();
    }
    if (config != h.config) h.retryMs = 0;
    h.config = config;
    if (!want || h.out || !h.socket || nowMs() < h.retryMs) {
        if (h.out && nowMs() >= h.nextLogMs) {
            h.nextLogMs = nowMs() + 30000;
            const auto st = h.out->stats();
            const auto net = h.socket->stats();
            std::cout << h.output << ": " << st.frames << " frames (" << st.keyframes << " forced key), "
                      << st.poolEmpty << " dropped, " << net.datagrams << " datagrams, "
                      << static_cast<double>(net.bytes) / 1e6 << " MB, keyframe requests " << net.keyframeRequests
                      << ", rtt " << net.rttMs << " ms\n"
                      << std::flush;
        }
        return;
    }
    h.lease = video_->acquire(config.mix, config.next);  // null while the mix does not exist: tried again next pass
    if (!h.lease) return;
    try {
        h.out = std::make_unique<LanVideoOutput>(*h.socket, config, h.lease->device());
    } catch (const std::exception& e) {
        std::cout << h.output << ": cannot open an encoder: " << e.what() << "\n" << std::flush;
        h.lease.reset();
        h.retryMs = nowMs() + 5000;
        return;
    }
    for (const auto& line : h.out->log()) std::cout << "  " << line << "\n";
    std::cout << h.output << ": mix " << config.mix << (config.next ? " (next scene)" : "") << " as " << h.out->description() << " from UDP "
              << h.socket->localPort() << "\n"
              << std::flush;
    h.nextLogMs = nowMs() + 30000;
    auto* out = h.out.get();
    h.consumer = h.lease->addFrameConsumer([out](const MixFrame& f) { out->onFrame(f); });
}

void EngineFollower::dropLan(LanHost& h) {
    if (h.lease && h.consumer) h.lease->removeConsumer(h.consumer);
    h.consumer = 0;
    h.out.reset();
    h.lease.reset();
    if (h.socket) h.socket->setDestinations({});
    h.viewers = 0;
    h.config = {};
}
#endif

void EngineFollower::reportOutput(const std::string& output, const std::string& state, const std::string& detail) {
    auto& last = reportedOutputs_[output];
    if (last.first == state && last.second == detail) return;
    last = {state, detail};
    proto::Args args{{"output", output}, {"state", state}};
    if (!detail.empty()) args["detail"] = detail;
    client_->call("/video/output/report", std::move(args));  // the answer is not needed
}

int EngineFollower::run(const std::atomic<bool>& stop) {
    std::cout << "capd: following the engine at " << options_.host << ":" << options_.port
              << " (Ctrl+C to exit)\n"
              << std::flush;
    while (!stop) {
        if (ensureConnected()) {
            followVideo();
            const auto state = client_->tree().getAs<std::string>("/capture/state").value_or("idle");
            if (state != lastState_) {
                lastState_ = state;
                onState(state);
            }
            checkSegment();
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
    recordSettings_ = *settings;
#ifndef _WIN32
    recordSettings_.withVideo = false;
#endif
    if (!options_.withVideo) recordSettings_.withVideo = false;
    const std::string path = unusedPath(recordingPath(recordSettings_, std::time(nullptr)));
    if (openRecorder(path)) report("recording", path, "", 0);
}

bool EngineFollower::openRecorder(const std::string& path) {
    const RecordSettings& s = recordSettings_;
    try {
#ifdef _WIN32
        if (s.withVideo) {
            if (!video_) throw std::runtime_error("the video mixer is not available");
            if (!mix_) {
                int mixNo = 1;
                if (auto v = client_->tree().getAs<std::int64_t>("/capture/mix")) mixNo = static_cast<int>(*v);
                mix_ = video_->acquire(mixNo);
                if (!mix_) throw std::runtime_error("video mix " + std::to_string(mixNo) + " does not exist");
            }
            recordDevice_ = mix_->device().device.Get();
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
        return false;
    }
    for (const auto& line : recorder_->log()) std::cout << line << "\n";
    std::cout << "recording " << path << "\n" << std::flush;
    return true;
}

// Design doc (disconnect table): after a GPU reset the recording closes its file and goes on in a new one; the gap
// is the few seconds the video engine needs to recreate the device.
void EngineFollower::checkSegment() {
#ifdef _WIN32
    if (!mix_ || (!recorder_ && !segmentPending_)) return;
    const std::string mixError = mix_->error();
    const bool lost = mixError.rfind("GPU lost", 0) == 0;
    const void* device = mix_->device().device.Get();
    if (recorder_ && (lost || device != recordDevice_)) {
        const auto fin = recorder_->stop();
        recorder_.reset();
        std::cout << "GPU reset: closed " << fin.file << " (" << fin.elapsedMs << " ms)\n" << std::flush;
        segmentPending_ = true;
        segmentSinceMs_ = nowMs();
    }
    if (!segmentPending_) return;
    if (!lost && device != recordDevice_) {
        segmentPending_ = false;
        const std::string path = unusedPath(recordingPath(recordSettings_, std::time(nullptr)));
        if (openRecorder(path)) report("recording", path, "", 0, "gpu_reset");
    } else if (nowMs() - segmentSinceMs_ > 15000) {
        segmentPending_ = false;
        mix_.reset();
        report("error", "", "the GPU did not come back after a reset: " + mixError, 0);
    }
#endif
}

void EngineFollower::stopRecording(bool doReport) {
#ifdef _WIN32
    if (segmentPending_) {
        segmentPending_ = false;
        mix_.reset();
    }
#endif
    if (!recorder_) {
        if (doReport) report("idle", "", "", 0);
        return;
    }
    const auto fin = recorder_->stop();
    recorder_.reset();
#ifdef _WIN32
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
                            std::int64_t durationMs, const std::string& reason) {
    if (!client_ || !client_->connected()) return;
    proto::Args args{{"state", state}};
    if (!file.empty()) args["file"] = file;
    if (!error.empty()) args["error"] = error;
    if (state == "idle") args["duration_ms"] = durationMs;
    if (!reason.empty()) args["reason"] = reason;
    auto result = client_->call("/capture/report", std::move(args));
    if (result.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        std::cout << "engine did not answer /capture/report\n" << std::flush;
        return;
    }
    const auto r = result.get();
    if (r.code != proto::ErrorCode::Ok) std::cout << "engine refused /capture/report: " << r.message << "\n" << std::flush;
}

}  // namespace dc::capd
