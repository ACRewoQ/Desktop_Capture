#include "controller.hpp"
#include "lan_service.hpp"

#include "dc/protocol/constants.hpp"
#include "dc/control/video_methods.hpp"
#include "dc/protocol/defaults.hpp"
#include "dc/protocol/video.hpp"
#include "dc/shmaudio/clock.hpp"
#include "dc/version.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <optional>
#include <thread>

namespace dc::mixd {

using namespace dc::proto;
using control::CallContext;

namespace {

// "HH:MM:SS " in local time, so log lines can be matched with what someone heard
std::string clockStamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S ", &tm);
    return buf;
}

Result error(ErrorCode code, std::string message) {
    Result r;
    r.code = code;
    r.message = std::move(message);
    return r;
}

std::optional<std::int64_t> intArg(const Args& args, const char* key) {
    if (auto it = args.find(key); it != args.end())
        if (auto* v = std::get_if<std::int64_t>(&it->second)) return *v;
    return std::nullopt;
}

std::string trackBase(int n) { return "/track/" + std::to_string(n); }

std::string apiOf(const DeviceInfo& d) { return d.asio ? "asio" : d.virtualCable ? "vmic" : "wasapi"; }

bool samePorts(const PortTable& a, const PortTable& b) { return a.inputs == b.inputs && a.outputs == b.outputs; }

} // namespace

Controller::Controller(asio::io_context& io, control::ControlServer& server, DeviceManager& devices, Engine& engine,
                       shm::AudioRingWriter* shm, Options options)
    : io_(io), server_(server), devices_(devices), engine_(engine), shm_(shm), options_(options), meterTimer_(io),
      statsTimer_(io), recordTimer_(io), recorderTimer_(io),
      fx_(io, server, options.plugins, [this] { scheduleRebuild(); }, options.verbose) {
    loader_ = std::make_unique<SampleLoader>([this](SampleLoader::Result r) {
        asio::post(io_, [this, r = std::move(r)]() mutable {
            if (!stopped_) sampleLoaded(std::move(r));
        });
    });
}

void Controller::start(const std::vector<DeviceInfo>& devices) {
    seed(devices);
    registerMethods();
    fx_.start();
    scheduleRebuild();
    scheduleMeters();
    scheduleStats();
}

void Controller::stop() {
    asio::post(io_, [this] {
        stopped_ = true;
        fx_.stop();
        meterTimer_.cancel();
        statsTimer_.cancel();
        recordTimer_.cancel();
        recorderTimer_.cancel();
        if (take_) {
            take_->finish(); // the files are complete even if the engine keeps running a little longer
            take_.reset();
        }
    });
}

std::vector<Change> Controller::deviceChanges(const std::vector<DeviceInfo>& devices) const {
    std::vector<Change> c;
    int inputs = 0, outputs = 0;
    for (const auto& d : devices) {
        const int n = d.input ? ++inputs : ++outputs;
        const std::string base = std::string("/devices/") + (d.input ? "input/" : "output/") + std::to_string(n);
        c.push_back({base + "/id", d.id});
        c.push_back({base + "/name", d.name});
        c.push_back({base + "/channels", std::int64_t{std::clamp(d.channels, 0, 64)}});
        c.push_back({base + "/connected", d.connected});
        c.push_back({base + "/api", apiOf(d)});
        c.push_back({base + "/sample_rate", std::int64_t{std::clamp(d.sampleRate, 0, 384000)}});
    }
    return c;
}

void Controller::seed(const std::vector<DeviceInfo>& devices) {
    std::vector<Change> c = globalDefaults();
    auto put = [&](const std::string& path, Value v) {
        for (auto& x : c)
            if (x.path == path) {
                x.value = std::move(v);
                return;
            }
        c.push_back({path, std::move(v)});
    };
    put("/system/version", std::string("mixd ") + kVersionString);
    put("/system/protocol", std::int64_t{kProtocolVersion});

    for (const auto& x : deviceChanges(devices)) c.push_back(x);
    deviceList_ = devices;
    for (const auto& d : devices) known_[d.id] = {d.name, d.input};

    for (const auto& m : masterDefaults()) c.push_back(m);
    for (const auto& d : devices)
        if (!d.input && d.isDefault) put("/master/out/1/target", "device/" + d.id);
    put("/master/meter_id", std::int64_t{0});

    for (int n = 1; n <= kInitialTracks; ++n) {
        for (const auto& t : trackDefaults(n)) c.push_back(t);
        put(trackBase(n) + "/position", std::int64_t{n});
    }
    put("/mixer/track_count", std::int64_t{kInitialTracks});
    put("/capture/audio/1/source", std::string("master"));
    server_.publishNow(c);
    server_.publishNow(videoDerived(server_.store().tree()));
}

void Controller::registerMethods() {
    server_.registerMethod("/mixer/track/add", [this](CallContext& ctx) { return addTrack(ctx); });
    server_.registerMethod("/mixer/track/remove", [this](CallContext& ctx) { return removeTrack(ctx); });
    server_.registerMethod("/mixer/track/move", [this](CallContext& ctx) { return moveTrack(ctx); });
    server_.registerMethod("/capture/record/start", [this](CallContext& ctx) { return startRecording(ctx); });
    server_.registerMethod("/capture/record/stop", [this](CallContext& ctx) { return stopRecording(ctx); });
    server_.registerMethod("/capture/report", [this](CallContext& ctx) { return captureReport(ctx); });
    server_.registerMethod("/video/output/report", [this](CallContext& ctx) { return outputReport(ctx); });
    server_.registerMethod("/recorder/start", [this](CallContext&) {
        std::string folder;
        auto r = startTake(false, &folder);
        if (r.code == ErrorCode::Ok) r.value = folder;
        return r;
    });
    server_.registerMethod("/recorder/stop", [this](CallContext&) {
        if (!take_) return error(ErrorCode::Conflict, "not recording");
        stopTake({});
        return Result{};
    });
    server_.registerMethod("/sampler/trigger",
                           [this](CallContext& ctx) { return samplerCommand(ctx, Sampler::Command::Type::Trigger); });
    server_.registerMethod("/sampler/release",
                           [this](CallContext& ctx) { return samplerCommand(ctx, Sampler::Command::Type::Release); });
    server_.registerMethod("/sampler/stop", [this](CallContext& ctx) {
        const auto bank = intArg(ctx.call.args, "bank");
        if (bank && (*bank < 1 || *bank > kSamplerBanks)) return error(ErrorCode::OutOfRange, "bank must be 1 to 8");
        Sampler::Command c;
        c.type = bank ? Sampler::Command::Type::StopBank : Sampler::Command::Type::StopAll;
        c.index = static_cast<std::int16_t>(bank.value_or(0));
        if (!engine_.mixer().sampler().post(c)) return error(ErrorCode::Internal, "sampler queue full");
        return Result{};
    });
    server_.registerMethod("/devices/refresh", [this](CallContext&) {
        devices_.refresh();
        return Result{};
    });
    control::registerVideoMethods(server_); // the video mixer's tree; capd does the work
}

namespace {
bool endsWith(const std::string& s, const char* suffix) {
    const std::size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
// "/sampler/bank/B/pad/P/<leaf>" -> pad index, -1 otherwise
int padOf(const std::string& path, const char* leaf) {
    int bank = 0, pad = 0;
    char rest[32] = {};
    if (std::sscanf(path.c_str(), "/sampler/bank/%d/pad/%d/%31s", &bank, &pad, rest) != 3) return -1;
    if (std::string(rest) != leaf || bank < 1 || bank > kSamplerBanks || pad < 1 || pad > kSamplerPadsPerBank) return -1;
    return padIndex(bank, pad);
}
std::string padBase(int index) {
    return "/sampler/bank/" + std::to_string(index / kSamplerPadsPerBank + 1) + "/pad/" +
           std::to_string(index % kSamplerPadsPerBank + 1);
}
} // namespace

void Controller::onDelta(const Delta& delta) {
    control::followVideoDelta(server_, delta);
    if (options_.lan) options_.lan->onDelta(delta); // first, so new LAN ports are there for the plan
    fx_.onDelta(delta);
    // values the engine itself reports (load, counters, device lists, derived states) and settings
    // read elsewhere (recorder, sampler files) do not change the mix; everything else might. Effect slots reach the mix through FxManager.
    auto reportOnly = [](const std::string& path) {
        if (FxManager::handlesPath(path)) return true;
        static const char* const kPrefixes[] = {"/system/cpu_load", "/system/xruns", "/system/latency_ms",
                                                "/system/clock_active", "/devices/", "/mixer/solo_active",
                                                "/capture/state", "/capture/elapsed_ms", "/capture/file",
                                                "/capture/error", "/video/", "/lan/", "/remote/", "/recorder/"};
        for (const char* p : kPrefixes)
            if (path.rfind(p, 0) == 0) return true;
        // of the sampler only the pads' mode and volume reach the plan (loaded files arrive separately)
        if (path.rfind("/sampler/", 0) == 0) return !endsWith(path, "/mode") && !endsWith(path, "/volume_db");
        return endsWith(path, "/input/state") || endsWith(path, "/input/lan_delay_ms");
    };
    for (const auto& c : delta.changes) {
        if (c.path == "/capture/state") {
            if (const auto* s = std::get_if<std::string>(&c.value))
                asio::post(io_, [this, state = *s] {
                    if (!stopped_) captureStateChanged(state);
                });
        } else if (const int pad = padOf(c.path, "file"); pad >= 0) {
            if (const auto* s = std::get_if<std::string>(&c.value))
                asio::post(io_, [this, pad, file = *s] {
                    if (!stopped_) padFileChanged(pad, file);
                });
        }
    }
    bool relevant = !delta.removed.empty() &&
                    std::any_of(delta.removed.begin(), delta.removed.end(), [&](const std::string& r) {
                        return !FxManager::handlesPath(r) && !reportOnly(r + "/");
                    });
    for (const auto& c : delta.changes) relevant = relevant || !reportOnly(c.path);
    if (relevant) scheduleRebuild();
}

void Controller::scheduleRebuild() {
    if (rebuildPending_) return;
    rebuildPending_ = true;
    // coalesce: a burst of Sets (a dragged fader) produces one plan per pass of the network thread
    asio::post(io_, [this] {
        rebuildPending_ = false;
        if (!stopped_) rebuild();
    });
}

void Controller::rebuild() {
    const auto& tree = server_.store().tree();
    devices_.setDemand(deviceDemand(tree));
    PortTable ports = ports_;
    if (options_.lan) options_.lan->addPorts(ports);
    auto result = buildPlan(tree, ports, ++generation_, options_.build, PlanResources{take_, samplerTable(), &fx_.rack()});
    engine_.setPlan(std::move(result.plan));
    engine_.collectRetired();

    if (shm_) {
        for (int slot = 0; slot < kMaxStrips; ++slot) {
            const auto ch = result.shmChannels[static_cast<std::size_t>(slot)];
            if (ch != shmChannels_[static_cast<std::size_t>(slot)]) {
                const std::string label = slot == 0 ? "master" : "track/" + std::to_string(slot);
                shm_->configureSlot(static_cast<std::uint32_t>(slot), ch, label.c_str());
                shmChannels_[static_cast<std::size_t>(slot)] = ch;
            }
            const bool disc = result.shmDisconnected[static_cast<std::size_t>(slot)];
            if (disc != shmDisconnected_[static_cast<std::size_t>(slot)]) {
                shm_->setSlotFlags(static_cast<std::uint32_t>(slot), disc ? shm::kSlotFlagSourceDisconnected : 0u);
                shmDisconnected_[static_cast<std::size_t>(slot)] = disc;
            }
        }
    }

    // derived read-only values; publishing unchanged values produces no delta, so this cannot loop
    std::vector<Change> c;
    for (const auto& [n, state] : result.inputState) c.push_back({trackBase(n) + "/input/state", state});
    c.push_back({"/mixer/solo_active", result.soloActive});
    server_.publishNow(c);
}

void Controller::onDeviceState(const DeviceManager::State& state) {
    asio::post(io_, [this, state] {
        if (!stopped_) applyDeviceState(state);
    });
}

void Controller::applyDeviceState(const DeviceManager::State& state) {
    if (state.devices != deviceList_) {
        publishDevices(state.devices);
        rematch();
    }
    if (state.processes != processList_) publishProcesses(state.processes);
    for (const auto& e : state.events) {
        if (options_.verbose)
            std::cout << clockStamp() << e.name << ": " << e.kind << " " << e.label << " [" << e.id << "]" << std::endl;
        server_.broadcastEventNow({e.name, {{"id", e.id}, {"name", e.label}, {"kind", e.kind}}});
    }
    server_.publishNow({{"/system/clock_active", state.clockActive},
                        {"/system/latency_ms", std::round(state.latencyMs * 100.0) / 100.0}});
    if (state.errors != lastErrors_) {
        for (const auto& e : state.errors) {
            if (std::find(lastErrors_.begin(), lastErrors_.end(), e) != lastErrors_.end()) continue;
            if (options_.verbose) std::cout << clockStamp() << "device error: " << e << std::endl;
            server_.broadcastEventNow({"device.error", {{"message", e}}});
        }
        lastErrors_ = state.errors;
    }
    if (state.notes != lastNotes_) {
        if (options_.verbose)
            for (const auto& n : state.notes)
                if (std::find(lastNotes_.begin(), lastNotes_.end(), n) == lastNotes_.end())
                    std::cout << clockStamp() << "device note: " << n << std::endl;
        lastNotes_ = state.notes;
    }
    if (!samePorts(state.ports, ports_)) {
        ports_ = state.ports;
        scheduleRebuild();
    }
}

void Controller::rematch() {
    const auto r = rematchDevices(server_.store().tree(), deviceList_, known_);
    if (r.changes.empty()) return;
    server_.publishNow(r.changes);
    for (const auto& m : r.moved) {
        if (options_.verbose)
            std::cout << clockStamp() << "device rematched by name: " << m.name << " [" << m.oldId << "] -> [" << m.newId << "]"
                      << std::endl;
        server_.broadcastEventNow({"device.rematched", {{"old_id", m.oldId}, {"id", m.newId}, {"name", m.name}}});
    }
    scheduleRebuild();
}

void Controller::publishProcesses(const std::vector<ProcessInfo>& processes) {
    processList_ = processes;
    std::vector<Change> c;
    int n = 0;
    for (const auto& p : processes) {
        const std::string base = "/devices/process/" + std::to_string(++n);
        c.push_back({base + "/exe", p.exe});
        c.push_back({base + "/pid", static_cast<std::int64_t>(p.pid)});
        c.push_back({base + "/title", p.title});
    }
    server_.publishNow(c, {"/devices/process"});
}

void Controller::publishDevices(const std::vector<DeviceInfo>& devices) {
    deviceList_ = devices;
    for (const auto& d : devices) known_[d.id] = {d.name, d.input};
    server_.publishNow(deviceChanges(devices), {"/devices/input", "/devices/output"});
    if (options_.verbose) {
        std::cout << "devices:" << std::endl;
        for (const auto& d : devices)
            std::cout << "  " << (d.input ? "in  " : "out ") << (d.connected ? "  " : "x ") << d.name << " [" << d.id << "]"
                      << std::endl;
    }
}

void Controller::scheduleMeters() {
    meterTimer_.expires_after(options_.meterInterval);
    meterTimer_.async_wait([this](asio::error_code ec) {
        if (ec || stopped_) return;
        const auto& tree = server_.store().tree();
        MeterPacket p;
        p.seq = meterSeq_++;
        p.timeUs = static_cast<std::uint64_t>(static_cast<double>(shm::nowQpc()) * 1e6 /
                                              static_cast<double>(shm::qpcFrequency()));
        auto entry = [&](int id, const std::string& base) {
            const auto r = engine_.mixer().meters().take(id);
            MeterEntry e;
            e.meterId = static_cast<std::uint16_t>(tree.getAs<std::int64_t>(base + "/meter_id").value_or(id));
            e.channels = id == 0 ? 2 : static_cast<std::uint8_t>(tree.getAs<std::int64_t>(base + "/channels").value_or(2));
            for (int c = 0; c < e.channels; ++c) {
                e.peakCdb[c] = linearToCdb(r.peak[c]);
                e.rmsCdb[c] = linearToCdb(r.rms[c]);
            }
            if (r.clipped) e.flags |= kMeterFlagClip;
            if (id != 0 && tree.getAs<std::string>(base + "/input/state") == "disconnected") e.flags |= kMeterFlagDisconnected;
            p.entries.push_back(e);
        };
        entry(0, "/master");
        for (int n : tracksByPosition(tree)) entry(n, trackBase(n));
        server_.sendMeters(std::move(p));
        publishPlaying();
        scheduleMeters();
    });
}

void Controller::scheduleStats() {
    statsTimer_.expires_after(options_.statsInterval);
    statsTimer_.async_wait([this](asio::error_code ec) {
        if (ec || stopped_) return;
        engine_.collectRetired();
        if (options_.lan) options_.lan->tick();
        const auto glitches = devices_.glitches();
        server_.publishNow({{"/system/cpu_load", std::round(engine_.cpuLoad() * 100.0) / 100.0},
                            {"/system/xruns", static_cast<std::int64_t>(glitches)}});
        if (glitches != loggedGlitches_) {
            // where the dropouts come from (device, clock bridge), for diagnosis
            std::cout << clockStamp() << "xruns " << glitches << " (+" << (glitches - loggedGlitches_) << ")" << std::endl;
            for (const auto& line : devices_.glitchDetails()) std::cout << "  " << line << std::endl;
            loggedGlitches_ = glitches;
        }
        scheduleStats();
    });
}

// --- recording: the engine owns the state, capd records (docs/spec/protocol.md 5.4.1) ---

namespace {
bool isCapd(const control::ClientInfo& c) { return c.role == ClientRole::Service && c.name == "capd"; }
std::optional<std::string> strArg(const Args& args, const char* key) {
    if (auto it = args.find(key); it != args.end())
        if (auto* v = std::get_if<std::string>(&it->second)) return *v;
    return std::nullopt;
}
} // namespace

void Controller::onClient(const control::ClientInfo& client, bool connected) {
    if (options_.lan) options_.lan->onClient(client, connected);
    if (!isCapd(client)) return;
    captureClients_ += connected ? 1 : -1;
    if (connected || captureClients_ > 0) return;
    server_.publishNow({{"/video/vcam/state", std::string("off")},
                        {"/video/vcam/detail", std::string()},
                        {"/video/monitor/state", std::string("off")},
                        {"/video/monitor/detail", std::string()}});
    const auto state = server_.store().tree().getAs<std::string>("/capture/state").value_or("idle");
    if (state == "starting" || state == "recording" || state == "stopping") {
        recordTimer_.cancel();
        server_.publishNow({{"/capture/state", std::string("error")},
                            {"/capture/error", std::string("capture process disconnected")}});
        server_.broadcastEventNow({"capture.stopped", {{"file", server_.store().tree().getAs<std::string>("/capture/file").value_or("")},
                                                       {"error", std::string("capture process disconnected")}}});
    }
}

Result Controller::startRecording(CallContext& ctx) {
    if (captureClients_ <= 0) return error(ErrorCode::Conflict, "capture process not running");
    const auto state = server_.store().tree().getAs<std::string>("/capture/state").value_or("idle");
    if (state != "idle" && state != "error") return error(ErrorCode::Conflict, "already recording (" + state + ")");
    server_.publishNow({{"/capture/state", std::string("starting")}, {"/capture/elapsed_ms", std::int64_t{0}}}, {},
                       ctx.call.id, ctx.client.name);
    return {};
}

Result Controller::stopRecording(CallContext& ctx) {
    const auto state = server_.store().tree().getAs<std::string>("/capture/state").value_or("idle");
    if (state != "recording") return error(ErrorCode::Conflict, "not recording");
    server_.publishNow({{"/capture/state", std::string("stopping")}}, {}, ctx.call.id, ctx.client.name);
    return {};
}

Result Controller::captureReport(CallContext& ctx) {
    if (ctx.client.role != ClientRole::Service) return error(ErrorCode::Unauthorized, "only the capture process reports");
    const auto& args = ctx.call.args;
    const auto state = strArg(args, "state");
    if (!state || (*state != "recording" && *state != "idle" && *state != "error"))
        return error(ErrorCode::BadRequest, "state must be recording, idle or error");
    const auto& tree = server_.store().tree();
    const auto file = strArg(args, "file").value_or(tree.getAs<std::string>("/capture/file").value_or(""));
    if (*state == "recording" && tree.getAs<std::string>("/capture/state") == "recording" && strArg(args, "reason")) {
        // A new file segment (GPU reset ...): the recording goes on, only the file changes.
        const auto reason = *strArg(args, "reason");
        server_.publishNow({{"/capture/file", file}}, {}, ctx.call.id, ctx.client.name);
        server_.broadcastEventNow({"capture.segment", {{"file", file}, {"reason", reason}}});
    } else if (*state == "recording") {
        recordStart_ = std::chrono::steady_clock::now();
        server_.publishNow({{"/capture/state", std::string("recording")},
                            {"/capture/file", file},
                            {"/capture/error", std::string()},
                            {"/capture/elapsed_ms", std::int64_t{0}}},
                           {}, ctx.call.id, ctx.client.name);
        server_.broadcastEventNow({"capture.started", {{"file", file}}});
        scheduleRecordingClock();
    } else if (*state == "idle") {
        recordTimer_.cancel();
        const auto elapsed = intArg(args, "duration_ms")
                                 .value_or(tree.getAs<std::int64_t>("/capture/elapsed_ms").value_or(0));
        server_.publishNow({{"/capture/state", std::string("idle")}, {"/capture/file", file}, {"/capture/elapsed_ms", elapsed}},
                           {}, ctx.call.id, ctx.client.name);
        server_.broadcastEventNow({"capture.stopped", {{"file", file}, {"duration_ms", elapsed}}});
    } else {
        recordTimer_.cancel();
        const auto message = strArg(args, "error").value_or("capture failed");
        server_.publishNow({{"/capture/state", std::string("error")}, {"/capture/error", message}}, {}, ctx.call.id,
                           ctx.client.name);
    }
    return {};
}

Result Controller::outputReport(CallContext& ctx) {
    if (ctx.client.role != ClientRole::Service) return error(ErrorCode::Unauthorized, "only the capture process reports");
    const auto& args = ctx.call.args;
    const auto output = strArg(args, "output");
    if (!output || (*output != "vcam" && *output != "monitor"))
        return error(ErrorCode::BadRequest, "output must be vcam or monitor");
    const auto state = strArg(args, "state").value_or("");
    const std::string base = "/video/" + *output;
    const auto& schema = server_.store().schema();
    const auto* spec = schema.find(base + "/state");
    if (!spec || schema.validate(*spec, Value(state), false)) return error(ErrorCode::BadRequest, "unknown " + *output + " state: " + state);
    server_.publishNow({{base + "/state", state}, {base + "/detail", strArg(args, "detail").value_or("")}}, {},
                       ctx.call.id, ctx.client.name);
    return {};
}

void Controller::scheduleRecordingClock() {
    recordTimer_.expires_after(std::chrono::milliseconds(250));
    recordTimer_.async_wait([this](asio::error_code ec) {
        if (ec || stopped_) return;
        if (server_.store().tree().getAs<std::string>("/capture/state") != "recording") return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                   recordStart_)
                                 .count();
        server_.publishNow({{"/capture/elapsed_ms", std::int64_t{elapsed}}});
        scheduleRecordingClock();
    });
}

// --- methods (same behaviour as the mock server, see tools/mock_server/mock_engine.cpp) ---

Result Controller::addTrack(CallContext& ctx) {
    const auto& args = ctx.call.args;
    const auto& tree = server_.store().tree();
    int id = 0;
    for (int n = 1; n <= kMaxTracks; ++n)
        if (!tree.contains(trackBase(n) + "/position")) {
            id = n;
            break;
        }
    if (id == 0) return error(ErrorCode::LimitReached, "64 tracks already exist");

    const std::int64_t channels = intArg(args, "channels").value_or(2);
    if (channels != 1 && channels != 2) return error(ErrorCode::OutOfRange, "channels must be 1 or 2");

    auto order = tracksByPosition(tree);
    const auto count = static_cast<std::int64_t>(order.size());
    const std::int64_t position = intArg(args, "position").value_or(count + 1);
    if (position < 1 || position > count + 1) return error(ErrorCode::OutOfRange, "position out of range");
    order.insert(order.begin() + (position - 1), id);

    std::vector<Change> c = trackDefaults(id);
    for (auto& x : c) {
        if (x.path == trackBase(id) + "/channels") x.value = channels;
        if (x.path == trackBase(id) + "/name")
            if (auto it = args.find("name"); it != args.end() && std::holds_alternative<std::string>(it->second))
                x.value = it->second;
    }
    for (std::size_t i = 0; i < order.size(); ++i)
        c.push_back({trackBase(order[i]) + "/position", static_cast<std::int64_t>(i + 1)});
    c.push_back({"/mixer/track_count", count + 1});
    server_.publishNow(c, {}, ctx.call.id, ctx.client.name);

    Result r;
    r.value = std::int64_t{id};
    return r;
}

Result Controller::removeTrack(CallContext& ctx) {
    const auto id = intArg(ctx.call.args, "track");
    const auto& tree = server_.store().tree();
    if (!id || !tree.contains(trackBase(static_cast<int>(*id)) + "/position")) return error(ErrorCode::NotFound, "no such track");
    const std::string ref = "track/" + std::to_string(*id);

    std::vector<Change> c;
    // clear every target and capture source that referenced the removed track
    for (const auto& [path, v] : tree.values()) {
        const auto* s = std::get_if<std::string>(&v);
        if (!s || *s != ref) continue;
        if (path.size() > 7 &&
            (path.compare(path.size() - 7, 7, "/target") == 0 || path.compare(path.size() - 7, 7, "/source") == 0))
            c.push_back({path, std::string()});
    }
    auto order = tracksByPosition(tree);
    order.erase(std::remove(order.begin(), order.end(), static_cast<int>(*id)), order.end());
    for (std::size_t i = 0; i < order.size(); ++i)
        c.push_back({trackBase(order[i]) + "/position", static_cast<std::int64_t>(i + 1)});
    c.push_back({"/mixer/track_count", static_cast<std::int64_t>(order.size())});
    server_.publishNow(c, {trackBase(static_cast<int>(*id))}, ctx.call.id, ctx.client.name);
    fx_.clearStrip(static_cast<int>(*id));
    return {};
}

Result Controller::moveTrack(CallContext& ctx) {
    const auto id = intArg(ctx.call.args, "track");
    const auto position = intArg(ctx.call.args, "position");
    auto order = tracksByPosition(server_.store().tree());
    if (!id || std::find(order.begin(), order.end(), static_cast<int>(*id)) == order.end())
        return error(ErrorCode::NotFound, "no such track");
    if (!position || *position < 1 || *position > static_cast<std::int64_t>(order.size()))
        return error(ErrorCode::OutOfRange, "position out of range");
    order.erase(std::remove(order.begin(), order.end(), static_cast<int>(*id)), order.end());
    order.insert(order.begin() + (*position - 1), static_cast<int>(*id));
    std::vector<Change> c;
    for (std::size_t i = 0; i < order.size(); ++i)
        c.push_back({trackBase(order[i]) + "/position", static_cast<std::int64_t>(i + 1)});
    server_.publishNow(c, {}, ctx.call.id, ctx.client.name);
    return {};
}


// --- per-track recording (docs/spec/protocol.md 5.4.2) ---

Result Controller::startTake(bool fromCapture, std::string* folderOut) {
    if (take_) return error(ErrorCode::Conflict, "already recording");
    const auto& tree = server_.store().tree();
    auto str = [&](const std::string& path) { return tree.getAs<std::string>(path).value_or(""); };

    std::vector<RecordStreamSpec> specs;
    auto arm = [&](int id, const std::string& base, const std::string& label) {
        if (!tree.getAs<bool>(base + "/record/enabled").value_or(false)) return;
        RecordStreamSpec s;
        s.strip = id;
        s.tap = str(base + "/record/tap") == "pre" ? Tap::Pre : Tap::Post;
        s.channels = s.tap == Tap::Pre && id != 0 ? static_cast<int>(tree.getAs<std::int64_t>(base + "/channels").value_or(2)) : 2;
        char num[8];
        std::snprintf(num, sizeof num, "%02d_", id);
        auto name = sanitizeFileName(str(base + "/name"));
        if (name.empty()) name = label;
        s.fileName = num + name + (str("/recorder/format") == "flac" ? ".flac" : ".wav");
        specs.push_back(std::move(s));
    };
    arm(0, "/master", "Master");
    for (int n : tracksByPosition(tree)) arm(n, trackBase(n), "Track " + std::to_string(n));
    if (specs.empty()) return error(ErrorCode::Conflict, "no track armed for recording");

    // <directory>/<folder_name>, made unique with _2, _3, ...
    std::string dir = str("/recorder/directory");
    if (dir.empty()) dir = defaultRecorderDirectory();
    char name[256] = {};
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    const auto pattern = str("/recorder/folder_name");
    if (pattern.empty() || std::strftime(name, sizeof name, pattern.c_str(), &tm) == 0) std::snprintf(name, sizeof name, "take");
    const auto base = pathFromUtf8(dir) / pathFromUtf8(name);
    auto folder = base;
    std::error_code ec;
    for (int i = 2; std::filesystem::exists(folder, ec); ++i) {
        folder = base;
        folder += "_" + std::to_string(i);
    }
    std::filesystem::create_directories(folder, ec);
    std::string err;
    if (ec) err = "cannot create folder " + pathToUtf8(folder) + ": " + ec.message();
    std::shared_ptr<RecorderTake> take;
    if (err.empty()) {
        take = RecorderTake::create(folder, str("/recorder/format") == "flac" ? AudioFileFormat::Flac : AudioFileFormat::Wav,
                                    std::move(specs), &err);
    }
    if (!take) {
        server_.publishNow({{"/recorder/state", std::string("error")}, {"/recorder/error", err}});
        if (options_.verbose) std::cout << clockStamp() << "recorder: " << err << std::endl;
        return error(ErrorCode::Internal, err);
    }

    take_ = take;
    takeFromCapture_ = fromCapture;
    rebuild(); // the take starts with the next block
    const auto folderText = pathToUtf8(folder);
    server_.publishNow({{"/recorder/state", std::string("recording")},
                        {"/recorder/folder", folderText},
                        {"/recorder/error", std::string()},
                        {"/recorder/elapsed_ms", std::int64_t{0}},
                        {"/recorder/dropped_frames", std::int64_t{0}}});
    server_.broadcastEventNow({"recorder.started", {{"folder", folderText}}});
    if (options_.verbose)
        std::cout << clockStamp() << "recorder: started " << folderText << " (" << take->streams().size() << " files)"
                  << std::endl;
    if (folderOut) *folderOut = folderText;
    scheduleRecorderClock();
    return {};
}

void Controller::stopTake(const std::string& reason) {
    auto take = std::move(take_);
    take_.reset();
    if (!take) return;
    recorderTimer_.cancel();
    rebuild();
    // let the engine adopt the plan without the take so every file ends on the same block
    const auto target = generation_;
    for (int i = 0; i < 500 && engine_.adoptedGeneration() < target; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool ok = take->finish();
    std::string err = reason;
    if (err.empty() && !ok) err = take->error();
    const auto durationMs = static_cast<std::int64_t>(take->fileFrames() * 1000 / 48000);
    const auto folderText = pathToUtf8(take->folder());
    server_.publishNow({{"/recorder/state", std::string(err.empty() ? "idle" : "error")},
                        {"/recorder/error", err},
                        {"/recorder/elapsed_ms", durationMs},
                        {"/recorder/dropped_frames", static_cast<std::int64_t>(take->dropped())}});
    Args data{{"folder", folderText}, {"duration_ms", durationMs}};
    if (!err.empty()) data["error"] = err;
    server_.broadcastEventNow({"recorder.stopped", data});
    if (options_.verbose)
        std::cout << clockStamp() << "recorder: stopped " << folderText << " " << durationMs << " ms, dropped "
                  << take->dropped() << (err.empty() ? "" : ", error: " + err) << std::endl;
}

void Controller::scheduleRecorderClock() {
    recorderTimer_.expires_after(std::chrono::milliseconds(250));
    recorderTimer_.async_wait([this](asio::error_code ec) {
        if (ec || stopped_ || !take_) return;
        if (take_->failed()) {
            stopTake(take_->error());
            return;
        }
        server_.publishNow({{"/recorder/elapsed_ms", static_cast<std::int64_t>(take_->frames() * 1000 / 48000)},
                            {"/recorder/dropped_frames", static_cast<std::int64_t>(take_->dropped())}});
        scheduleRecorderClock();
    });
}

void Controller::captureStateChanged(const std::string& state) {
    if (state == "recording") {
        if (!take_ && server_.store().tree().getAs<bool>("/recorder/with_capture").value_or(false)) {
            std::string folder;
            startTake(true, &folder); // nothing armed: the video records alone
        }
    } else if (take_ && takeFromCapture_) {
        stopTake({});
    }
}

// --- sampler (docs/spec/protocol.md 5.4.3) ---

void Controller::padFileChanged(int pad, const std::string& file) {
    auto& p = pads_[static_cast<std::size_t>(pad)];
    if (file == p.file && (p.sample || file.empty())) return;
    p.file = file;
    p.serial = ++loadSerial_;
    const auto base = padBase(pad);
    const bool hadSample = p.sample != nullptr;
    p.sample.reset();
    if (file.empty()) {
        server_.publishNow({{base + "/state", std::string("empty")}, {base + "/error", std::string()},
                            {base + "/duration_ms", std::int64_t{0}}});
    } else {
        server_.publishNow({{base + "/state", std::string("loading")}, {base + "/error", std::string()}});
        loader_->load(pad, file, p.serial);
    }
    if (hadSample) scheduleRebuild();
}

void Controller::sampleLoaded(SampleLoader::Result r) {
    auto& p = pads_[static_cast<std::size_t>(r.pad)];
    if (r.serial != p.serial) return; // replaced while loading
    const auto base = padBase(r.pad);
    if (!r.sample) {
        server_.publishNow({{base + "/state", std::string("error")}, {base + "/error", r.error},
                            {base + "/duration_ms", std::int64_t{0}}});
        if (options_.verbose) std::cout << clockStamp() << "sampler: " << r.file << ": " << r.error << std::endl;
        return;
    }
    p.sample = r.sample;
    server_.publishNow({{base + "/state", std::string("ready")}, {base + "/error", std::string()},
                        {base + "/duration_ms", static_cast<std::int64_t>(r.sample->frames * 1000 / 48000)}});
    scheduleRebuild();
}

std::shared_ptr<const SamplerTable> Controller::samplerTable() const {
    const auto& tree = server_.store().tree();
    auto table = std::make_shared<SamplerTable>();
    bool any = false;
    for (int i = 0; i < kSamplerPadCount; ++i) {
        const auto& p = pads_[static_cast<std::size_t>(i)];
        if (!p.sample) continue;
        any = true;
        const auto base = padBase(i);
        auto& pp = table->pads[static_cast<std::size_t>(i)];
        pp.sample = p.sample;
        pp.mode = padModeFromString(tree.getAs<std::string>(base + "/mode").value_or("oneshot"));
        double db = 0;
        if (const auto* v = tree.get(base + "/volume_db"))
            if (auto d = asNumber(*v)) db = *d;
        pp.gain = dbToLinear(db);
    }
    if (!any) return nullptr;
    return table;
}

void Controller::publishPlaying() {
    const auto bits = engine_.mixer().sampler().playing();
    if (bits == lastPlaying_) return;
    std::vector<Change> c;
    for (int i = 0; i < kSamplerPadCount; ++i) {
        const bool now = (bits[static_cast<std::size_t>(i / 64)] >> (i % 64)) & 1u;
        const bool before = (lastPlaying_[static_cast<std::size_t>(i / 64)] >> (i % 64)) & 1u;
        if (now != before) c.push_back({padBase(i) + "/playing", now});
    }
    lastPlaying_ = bits;
    server_.publishNow(c);
}

Result Controller::samplerCommand(CallContext& ctx, Sampler::Command::Type type) {
    const auto& args = ctx.call.args;
    const auto pad = intArg(args, "pad");
    if (!pad) return error(ErrorCode::BadRequest, "pad is required");
    const auto bank = intArg(args, "bank").value_or(server_.store().tree().getAs<std::int64_t>("/sampler/active_bank").value_or(1));
    if (*pad < 1 || *pad > kSamplerPadsPerBank) return error(ErrorCode::OutOfRange, "pad must be 1 to 16");
    if (bank < 1 || bank > kSamplerBanks) return error(ErrorCode::OutOfRange, "bank must be 1 to 8");
    Sampler::Command c;
    c.type = type;
    c.index = static_cast<std::int16_t>(padIndex(static_cast<int>(bank), static_cast<int>(*pad)));
    if (!engine_.mixer().sampler().post(c)) return error(ErrorCode::Internal, "sampler queue full");
    return {};
}

} // namespace dc::mixd
