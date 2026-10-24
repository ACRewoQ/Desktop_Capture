#include "controller.hpp"
#include "lan_service.hpp"

#include "dc/protocol/constants.hpp"
#include "dc/protocol/defaults.hpp"
#include "dc/shmaudio/clock.hpp"
#include "dc/version.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iostream>
#include <optional>

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

std::string apiOf(const DeviceInfo& d) { return d.virtualCable ? "vmic" : "wasapi"; }

bool samePorts(const PortTable& a, const PortTable& b) { return a.inputs == b.inputs && a.outputs == b.outputs; }

} // namespace

Controller::Controller(asio::io_context& io, control::ControlServer& server, DeviceManager& devices, Engine& engine,
                       shm::AudioRingWriter* shm, Options options)
    : io_(io), server_(server), devices_(devices), engine_(engine), shm_(shm), options_(options), meterTimer_(io),
      statsTimer_(io), recordTimer_(io), fx_(io, server, options.plugins, [this] { scheduleRebuild(); }, options.verbose) {}

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
}

void Controller::registerMethods() {
    server_.registerMethod("/mixer/track/add", [this](CallContext& ctx) { return addTrack(ctx); });
    server_.registerMethod("/mixer/track/remove", [this](CallContext& ctx) { return removeTrack(ctx); });
    server_.registerMethod("/mixer/track/move", [this](CallContext& ctx) { return moveTrack(ctx); });
    server_.registerMethod("/capture/record/start", [this](CallContext& ctx) { return startRecording(ctx); });
    server_.registerMethod("/capture/record/stop", [this](CallContext& ctx) { return stopRecording(ctx); });
    server_.registerMethod("/capture/report", [this](CallContext& ctx) { return captureReport(ctx); });
    server_.registerMethod("/devices/refresh", [this](CallContext&) {
        devices_.refresh();
        return Result{};
    });
}

void Controller::onDelta(const Delta& delta) {
    if (options_.lan) options_.lan->onDelta(delta); // first, so new LAN ports are there for the plan
    fx_.onDelta(delta);
    // values the engine itself reports (load, counters, device lists, derived states) do not change
    // the mix; everything else might. Effect slots reach the mix through FxManager.
    auto reportOnly = [](const std::string& path) {
        if (FxManager::handlesPath(path)) return true;
        static const char* const kPrefixes[] = {"/system/cpu_load", "/system/xruns", "/system/latency_ms",
                                                "/system/clock_active", "/devices/", "/mixer/solo_active",
                                                "/capture/state", "/capture/elapsed_ms", "/capture/file",
                                                "/capture/error", "/lan/", "/remote/"};
        for (const char* p : kPrefixes)
            if (path.rfind(p, 0) == 0) return true;
        auto endsWith = [&](std::string_view tail) {
            return path.size() > tail.size() && path.compare(path.size() - tail.size(), tail.size(), tail) == 0;
        };
        return endsWith("/input/state") || endsWith("/input/lan_delay_ms");
    };
    bool relevant = !delta.removed.empty() && std::any_of(delta.removed.begin(), delta.removed.end(), [&](const std::string& r) {
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
    auto result = buildPlan(tree, ports, ++generation_, options_.build, &fx_.rack());
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
    if (state.devices != deviceList_) publishDevices(state.devices);
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

void Controller::publishDevices(const std::vector<DeviceInfo>& devices) {
    deviceList_ = devices;
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
    if (*state == "recording") {
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

} // namespace dc::mixd
