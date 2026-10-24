#include "plan_builder.hpp"

#include "dsp.hpp"

#include "dc/protocol/routing.hpp"
#include "dc/protocol/schema.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>

namespace dc::mixd {

using proto::ParamTree;
using proto::RouteTarget;

namespace {

std::string trackBase(int n) { return "/track/" + std::to_string(n); }

double num(const ParamTree& t, const std::string& path, double def) {
    if (const auto* v = t.get(path))
        if (auto d = proto::asNumber(*v)) return *d;
    return def;
}
bool flag(const ParamTree& t, const std::string& path) { return t.getAs<bool>(path).value_or(false); }
std::string str(const ParamTree& t, const std::string& path) { return t.getAs<std::string>(path).value_or(""); }

bool trackExists(const ParamTree& t, int n) { return t.contains(trackBase(n) + "/position"); }

std::vector<int> existingTracks(const ParamTree& t) {
    std::vector<int> out;
    for (int n = 1; n <= proto::kMaxTracks; ++n)
        if (trackExists(t, n)) out.push_back(n);
    return out;
}

// Device id of a device or virtual microphone target, "" for anything else.
std::string deviceOf(const std::string& target) {
    auto t = proto::parseTarget(target);
    if (t && (t->kind == RouteTarget::Kind::Device || t->kind == RouteTarget::Kind::VirtualMic)) return t->id;
    return {};
}

} // namespace

std::vector<int> tracksByPosition(const ParamTree& tree) {
    std::vector<std::pair<double, int>> v;
    for (int n : existingTracks(tree)) v.push_back({num(tree, trackBase(n) + "/position", n), n});
    std::sort(v.begin(), v.end());
    std::vector<int> out;
    for (auto& [p, n] : v) out.push_back(n);
    return out;
}

DeviceDemand deviceDemand(const ParamTree& tree) {
    DeviceDemand d;
    d.clockDevice = str(tree, "/system/clock_device");
    d.mode = tree.getAs<std::string>("/system/audio_mode").value_or("shared");
    d.blockFrames = static_cast<int>(num(tree, "/system/block_size", 256));

    std::set<std::string> inputs;
    std::vector<std::string> outputs;
    auto addOut = [&](const std::string& id) {
        if (!id.empty() && std::find(outputs.begin(), outputs.end(), id) == outputs.end()) outputs.push_back(id);
    };
    if (!d.clockDevice.empty()) addOut(d.clockDevice);
    for (int k = 1; k <= proto::kMaxTrackOutputs; ++k) addOut(deviceOf(str(tree, "/master/out/" + std::to_string(k) + "/target")));

    std::set<std::string> rest;
    for (int n : existingTracks(tree)) {
        const auto base = trackBase(n);
        if (str(tree, base + "/input/kind") == "device") {
            const auto src = str(tree, base + "/input/source");
            if (!src.empty()) inputs.insert(src);
        }
        for (int k = 1; k <= proto::kMaxTrackOutputs; ++k)
            if (auto id = deviceOf(str(tree, base + "/out/" + std::to_string(k) + "/target")); !id.empty()) rest.insert(id);
        for (int k = 1; k <= proto::kMaxTrackSends; ++k)
            if (auto id = deviceOf(str(tree, base + "/send/" + std::to_string(k) + "/target")); !id.empty()) rest.insert(id);
    }
    if (auto id = deviceOf(str(tree, "/cue/target")); !id.empty()) rest.insert(id);
    for (const auto& id : rest) addOut(id);
    d.inputs.assign(inputs.begin(), inputs.end());
    d.outputs = std::move(outputs);
    return d;
}

BuildResult buildPlan(const ParamTree& tree, const PortTable& ports, std::uint64_t generation,
                      const BuildOptions& options, const FxRack* fx) {
    BuildResult r;
    auto plan = std::make_unique<MixPlan>();
    plan->generation = generation;
    plan->blockFrames = std::clamp(static_cast<int>(num(tree, "/system/block_size", 256)), 16, kMaxBlockFrames);
    plan->testToneGain = options.testToneGain;
    plan->testToneHz = options.testToneHz;

    const auto tracks = existingTracks(tree);
    std::set<int> trackSet(tracks.begin(), tracks.end());

    std::map<std::string, int> busIndex;
    auto bus = [&](const std::string& id) -> int {
        if (auto it = busIndex.find(id); it != busIndex.end()) return it->second;
        auto port = ports.outputs.find(id);
        if (port == ports.outputs.end() || static_cast<int>(plan->buses.size()) >= kMaxOutputBuses) return -1;
        const int i = static_cast<int>(plan->buses.size());
        plan->buses.push_back(port->second);
        busIndex[id] = i;
        return i;
    };
    std::map<std::string, int> inputIndex;
    auto input = [&](const std::string& id) -> int {
        if (auto it = inputIndex.find(id); it != inputIndex.end()) return it->second;
        auto port = ports.inputs.find(id);
        if (port == ports.inputs.end() || static_cast<int>(plan->inputs.size()) >= kMaxInputPorts) return -1;
        const int i = static_cast<int>(plan->inputs.size());
        plan->inputs.push_back(port->second);
        inputIndex[id] = i;
        return i;
    };
    // a target string -> where the audio goes; nullopt when it goes nowhere right now
    auto destination = [&](const std::string& target, bool allowTracks) -> std::optional<Destination> {
        auto t = proto::parseTarget(target);
        if (!t) return std::nullopt;
        switch (t->kind) {
            case RouteTarget::Kind::Master:
                if (allowTracks) return Destination{Destination::Kind::Master, 0};
                return std::nullopt;
            case RouteTarget::Kind::Track:
                if (allowTracks && trackSet.count(t->track)) return Destination{Destination::Kind::Track, t->track};
                return std::nullopt;
            case RouteTarget::Kind::Device:
            case RouteTarget::Kind::VirtualMic:
                if (int b = bus(t->id); b >= 0) return Destination{Destination::Kind::Bus, b};
                return std::nullopt;
            case RouteTarget::Kind::Lan:
                if (int n = proto::lanStreamOf(*t); n > 0)
                    if (int b = bus("lan:" + std::to_string(n)); b >= 0) return Destination{Destination::Kind::Bus, b};
                return std::nullopt; // nobody receives the stream right now
            default: return std::nullopt;
        }
    };

    // --- solo: a soloed track stays audible together with everything that feeds it and everything
    // it feeds (otherwise its sound could not reach the outputs); every other track is silenced.
    std::map<int, std::set<int>> feeds, fedBy;
    for (int n : tracks) {
        const auto base = trackBase(n);
        auto addEdge = [&](const std::string& target) {
            auto t = proto::parseTarget(target);
            if (t && t->kind == RouteTarget::Kind::Track && trackSet.count(t->track)) {
                feeds[n].insert(t->track);
                fedBy[t->track].insert(n);
            }
        };
        for (int k = 1; k <= proto::kMaxTrackOutputs; ++k) addEdge(str(tree, base + "/out/" + std::to_string(k) + "/target"));
        for (int k = 1; k <= proto::kMaxTrackSends; ++k) addEdge(str(tree, base + "/send/" + std::to_string(k) + "/target"));
    }
    std::set<int> audible;
    for (int n : tracks)
        if (flag(tree, trackBase(n) + "/solo")) {
            r.soloActive = true;
            std::function<void(int, std::map<int, std::set<int>>&)> walk = [&](int t, std::map<int, std::set<int>>& g) {
                for (int next : g[t])
                    if (audible.insert(next).second) walk(next, g);
            };
            audible.insert(n);
            walk(n, feeds);
            walk(n, fedBy);
        }

    auto strip = [&](const std::string& base, int id) {
        StripPlan s;
        s.id = id;
        s.channels = id == 0 ? 2 : static_cast<int>(num(tree, base + "/channels", 2));
        const bool silencedBySolo = id != 0 && r.soloActive && !audible.count(id);
        s.gain = flag(tree, base + "/mute") || silencedBySolo ? 0.0f : dbToLinear(num(tree, base + "/volume_db", 0));
        s.pan = static_cast<float>(num(tree, base + "/pan", 0));
        s.lowcut = flag(tree, base + "/lowcut/enabled");
        s.lowcutHz = num(tree, base + "/lowcut/freq_hz", 80);
        s.highcut = flag(tree, base + "/highcut/enabled");
        s.highcutHz = num(tree, base + "/highcut/freq_hz", 18000);
        s.meterId = static_cast<std::uint16_t>(num(tree, base + "/meter_id", id));
        if (fx) s.fx = (*fx)[static_cast<std::size_t>(id)];
        for (int k = 1; k <= proto::kMaxTrackOutputs; ++k) {
            const auto d = destination(str(tree, base + "/out/" + std::to_string(k) + "/target"), id != 0);
            if (!d) continue;
            const bool duplicate = std::any_of(s.outputs.begin(), s.outputs.end(), [&](const Destination& o) {
                return o.kind == d->kind && o.index == d->index;
            });
            if (!duplicate) s.outputs.push_back(*d); // the same destination twice would double the level
        }
        return s;
    };

    // --- tracks in processing order
    auto order = proto::processingOrder(tree);
    if (order.size() != tracks.size()) order = tracks; // a cycle cannot pass the validator; be safe anyway
    for (int n : order) {
        const auto base = trackBase(n);
        StripPlan s = strip(base, n);
        const auto kind = str(tree, base + "/input/kind");
        std::string state = "none";
        if (kind == "device") {
            const auto src = str(tree, base + "/input/source");
            s.input = src.empty() ? -1 : input(src);
            s.inputChannel = static_cast<int>(num(tree, base + "/input/channel", 1)) - 1;
            if (!src.empty()) state = s.input >= 0 ? "ok" : "disconnected";
            s.inputDisconnected = !src.empty() && s.input < 0;
        } else if (kind == "lan") {
            // LAN stream "<peer id>/<n>": the LAN service opens a port for it while it can subscribe
            const auto src = str(tree, base + "/input/source");
            s.input = src.empty() ? -1 : input("lan:" + src);
            s.inputChannel = static_cast<int>(num(tree, base + "/input/channel", 1)) - 1;
            const bool playing = s.input >= 0 && plan->inputs[static_cast<std::size_t>(s.input)]->streaming.load();
            if (!src.empty()) state = playing ? "ok" : "disconnected";
            s.inputDisconnected = !src.empty() && !playing;
        } else if (kind != "none" && !kind.empty()) {
            state = "disconnected"; // process / sampler inputs arrive in phase 3
            s.inputDisconnected = true;
        }
        r.inputState[n] = state;
        s.cue = flag(tree, base + "/cue");
        for (int k = 1; k <= proto::kMaxTrackSends; ++k) {
            const auto sk = base + "/send/" + std::to_string(k);
            const auto d = destination(str(tree, sk + "/target"), true);
            if (!d || d->kind == Destination::Kind::Master) continue; // sends never go to the master
            s.sends.push_back({*d, dbToLinear(num(tree, sk + "/level_db", 0)), flag(tree, sk + "/pre_fader"), k - 1});
        }
        plan->tracks.push_back(std::move(s));
    }
    plan->master = strip("/master", 0);

    if (auto id = deviceOf(str(tree, "/cue/target")); !id.empty()) {
        plan->cueBus = bus(id);
        plan->cueGain = dbToLinear(num(tree, "/cue/volume_db", 0));
    }

    // --- shared memory: the strips the capture output records
    auto stripById = [&](int id) -> StripPlan* {
        if (id == 0) return &plan->master;
        for (auto& t : plan->tracks)
            if (t.id == id) return &t;
        return nullptr;
    };
    for (int k = 1; k <= proto::kMaxCaptureAudioStreams; ++k) {
        auto src = proto::parseTarget(str(tree, "/capture/audio/" + std::to_string(k) + "/source"));
        if (!src) continue;
        int id = -1;
        if (src->kind == RouteTarget::Kind::Master) id = 0;
        else if (src->kind == RouteTarget::Kind::Track) id = src->track;
        StripPlan* s = id >= 0 ? stripById(id) : nullptr;
        if (!s) continue;
        const auto base = id == 0 ? std::string("/master") : trackBase(id);
        s->shmTap = str(tree, base + "/record/tap") == "pre" ? Tap::Pre : Tap::Post;
        r.shmChannels[static_cast<std::size_t>(id)] = s->shmTap == Tap::Pre ? static_cast<std::uint32_t>(s->channels) : 2u;
        r.shmDisconnected[static_cast<std::size_t>(id)] = s->inputDisconnected;
    }

    for (const auto& in : plan->inputs)
        plan->inputScratch.emplace_back(static_cast<std::size_t>(kMaxBlockFrames * in->channels), 0.0f);
    r.plan = std::move(plan);
    return r;
}

} // namespace dc::mixd
