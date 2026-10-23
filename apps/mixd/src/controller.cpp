#include "controller.hpp"

#include "dc/protocol/constants.hpp"
#include "dc/protocol/defaults.hpp"
#include "dc/shmaudio/clock.hpp"
#include "dc/version.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>

namespace dc::mixd {

using namespace dc::proto;
using control::CallContext;

namespace {

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
      statsTimer_(io) {}

void Controller::start(const std::vector<DeviceInfo>& devices) {
    seed(devices);
    registerMethods();
    scheduleRebuild();
    scheduleMeters();
    scheduleStats();
}

void Controller::stop() {
    asio::post(io_, [this] {
        stopped_ = true;
        meterTimer_.cancel();
        statsTimer_.cancel();
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
    server_.registerMethod("/devices/refresh", [this](CallContext&) {
        devices_.refresh();
        return Result{};
    });
}

void Controller::onDelta(const Delta&) { scheduleRebuild(); }

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
    auto result = buildPlan(tree, ports_, ++generation_, options_.build);
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
            if (options_.verbose) std::cout << "device error: " << e << std::endl;
            server_.broadcastEventNow({"device.error", {{"message", e}}});
        }
        lastErrors_ = state.errors;
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
        server_.publishNow({{"/system/cpu_load", std::round(engine_.cpuLoad() * 100.0) / 100.0},
                            {"/system/xruns", static_cast<std::int64_t>(devices_.glitches())}});
        scheduleStats();
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
