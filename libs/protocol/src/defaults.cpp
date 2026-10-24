#include "dc/protocol/defaults.hpp"

#include "dc/protocol/video.hpp"

#include <functional>

namespace dc::proto {

namespace {

bool startsWith(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// Expands every "#" of a pattern. The first "#" can be fixed by the caller (track number).
void expand(const std::string& pattern, const Value& value, int fixedFirst, std::vector<Change>& out) {
    std::function<void(std::size_t, std::string, bool)> rec = [&](std::size_t pos, std::string acc, bool firstUsed) {
        const auto hash = pattern.find('#', pos);
        if (hash == std::string::npos) {
            out.push_back({acc + pattern.substr(pos), value});
            return;
        }
        acc += pattern.substr(pos, hash - pos);
        if (!firstUsed && fixedFirst > 0) {
            rec(hash + 1, acc + std::to_string(fixedFirst), true);
            return;
        }
        // segment before '#': "/track/#/out/#" -> "out"
        const auto segEnd = hash - 1;
        const auto segStart = pattern.rfind('/', segEnd - 1) + 1;
        const int n = listSize(pattern.substr(segStart, segEnd - segStart));
        for (int i = 1; i <= n; ++i) rec(hash + 1, acc + std::to_string(i), true);
    };
    rec(0, {}, false);
}

bool isDynamic(const std::string& pattern) {
    return pattern.find("/fx/") != std::string::npos || pattern.find('*') != std::string::npos ||
           startsWith(pattern, "/video/source/") || startsWith(pattern, "/video/scene/") ||
           startsWith(pattern, "/video/devices/");
}

} // namespace

int listSize(const std::string& segment) {
    if (segment == "out") return kMaxTrackOutputs;
    if (segment == "send") return kMaxTrackSends;
    if (segment == "audio") return kMaxCaptureAudioStreams;
    if (segment == "mix") return 1; // one video mix by default; more are added by lane B1
    return 0;
}

std::vector<Change> trackDefaults(int track, const Schema& schema) {
    std::vector<Change> out;
    for (const auto& spec : schema.params()) {
        if (!startsWith(spec.pattern, "/track/#/") || isDynamic(spec.pattern)) continue;
        if (spec.pattern == "/track/#/position") continue;
        expand(spec.pattern, spec.defaultValue, track, out);
    }
    const std::string base = "/track/" + std::to_string(track);
    for (auto& c : out) {
        if (c.path == base + "/out/1/target") c.value = std::string("master");
        else if (c.path == base + "/name") c.value = "Track " + std::to_string(track);
        else if (c.path == base + "/meter_id") c.value = std::int64_t{track};
    }
    return out;
}

std::vector<Change> masterDefaults(const Schema& schema) {
    std::vector<Change> out;
    for (const auto& spec : schema.params()) {
        if (!startsWith(spec.pattern, "/master/") || isDynamic(spec.pattern)) continue;
        expand(spec.pattern, spec.defaultValue, 0, out);
    }
    return out;
}

std::vector<Change> globalDefaults(const Schema& schema) {
    std::vector<Change> out;
    for (const auto& spec : schema.params()) {
        const auto& p = spec.pattern;
        if (startsWith(p, "/track/") || startsWith(p, "/master/") || startsWith(p, "/devices/") || isDynamic(p))
            continue;
        expand(p, spec.defaultValue, 0, out);
    }
    for (auto& c : out) {
        if (c.path == "/video/mix/1/name") c.value = std::string("Mix A");
        else if (c.path == "/video/mix/1/program_scene" || c.path == "/video/mix/1/preview_scene") c.value = std::int64_t{1};
    }
    for (auto& c : videoStartDefaults(schema)) out.push_back(std::move(c));
    return out;
}

} // namespace dc::proto
