#include "dc/protocol/video.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <optional>

namespace dc::proto {

namespace {

bool startsWith(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

std::string sourceBase(int n) { return "/video/source/" + std::to_string(n); }
std::string sceneBase(int n) { return "/video/scene/" + std::to_string(n); }
std::string layerBase(int scene, int layer) { return sceneBase(scene) + "/layer/" + std::to_string(layer); }
std::string mixBase(int n) { return "/video/mix/" + std::to_string(n); }

bool sourceExists(const ParamTree& t, int n) { return n >= 1 && t.contains(sourceBase(n) + "/kind"); }
bool sceneExists(const ParamTree& t, int n) { return n >= 1 && t.contains(sceneBase(n) + "/position"); }
bool layerExists(const ParamTree& t, int s, int l) { return l >= 1 && t.contains(layerBase(s, l) + "/position"); }
bool mixExists(const ParamTree& t, int n) { return n >= 1 && t.contains(mixBase(n) + "/width"); }

// Expands the schema patterns under `prefix` (e.g. "/video/scene/#/layer/#/") whose "#" count equals
// numbers.size(), substituting the numbers in order.
std::vector<Change> expandFixed(const Schema& schema, const std::string& prefix, const std::vector<int>& numbers) {
    std::vector<Change> out;
    const auto hashes = static_cast<std::size_t>(std::count(prefix.begin(), prefix.end(), '#'));
    for (const auto& spec : schema.params()) {
        const auto& p = spec.pattern;
        if (!startsWith(p, prefix)) continue;
        if (static_cast<std::size_t>(std::count(p.begin(), p.end(), '#')) != hashes || hashes != numbers.size()) continue;
        std::string path;
        std::size_t k = 0;
        for (char c : p) {
            if (c == '#') path += std::to_string(numbers[k++]);
            else path += c;
        }
        out.push_back({path, spec.defaultValue});
    }
    return out;
}

void put(std::vector<Change>& c, const std::string& path, Value v) {
    for (auto& x : c)
        if (x.path == path) {
            x.value = std::move(v);
            return;
        }
    c.push_back({path, std::move(v)});
}

Result error(ErrorCode code, std::string message) {
    Result r;
    r.code = code;
    r.message = std::move(message);
    return r;
}

std::optional<std::int64_t> intArg(const Args& args, const std::string& key) {
    if (auto it = args.find(key); it != args.end())
        if (auto* v = std::get_if<std::int64_t>(&it->second)) return *v;
    return std::nullopt;
}

std::optional<std::string> strArg(const Args& args, const std::string& key) {
    if (auto it = args.find(key); it != args.end())
        if (auto* v = std::get_if<std::string>(&it->second)) return *v;
    return std::nullopt;
}

// Numbers n for which `prefix + n + suffix` exists, ascending ("/video/scene/", "/position").
std::vector<int> numbersUnder(const ParamTree& t, const std::string& prefix, const std::string& suffix) {
    std::vector<int> out;
    for (auto it = t.values().lower_bound(prefix); it != t.values().end() && startsWith(it->first, prefix); ++it) {
        const std::string rest = it->first.substr(prefix.size());
        const auto slash = rest.find('/');
        if (slash == std::string::npos || rest.compare(slash, std::string::npos, suffix) != 0) continue;
        bool digits = slash > 0;
        for (std::size_t i = 0; i < slash; ++i) digits = digits && std::isdigit(static_cast<unsigned char>(rest[i]));
        if (digits) out.push_back(std::atoi(rest.substr(0, slash).c_str()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<int> byPosition(const ParamTree& t, const std::vector<int>& ids, const std::function<std::string(int)>& base) {
    std::vector<std::pair<std::int64_t, int>> v;
    for (int id : ids) v.push_back({t.getAs<std::int64_t>(base(id) + "/position").value_or(0), id});
    std::sort(v.begin(), v.end());
    std::vector<int> out;
    for (auto& [p, id] : v) out.push_back(id);
    return out;
}

int firstFree(const std::function<bool(int)>& exists, int max) {
    for (int n = 1; n <= max; ++n)
        if (!exists(n)) return n;
    return 0;
}

void renumber(std::vector<Change>& c, const std::vector<int>& order, const std::function<std::string(int)>& base) {
    for (std::size_t i = 0; i < order.size(); ++i) c.push_back({base(order[i]) + "/position", static_cast<std::int64_t>(i + 1)});
}

// Paths that hold a mix number (the video outputs).
const char* const kMixRefs[] = {"/capture/mix", "/video/vcam/mix", "/video/monitor/mix", "/video/preview/mix",
                                 "/video/lan/mix"};

const char* defaultSourceName(const std::string& kind) {
    if (kind == "display") return "Display";
    if (kind == "window") return "Window";
    if (kind == "camera") return "Camera";
    if (kind == "image") return "Image";
    if (kind == "media") return "Media";
    if (kind == "text") return "Text";
    if (kind == "color") return "Color";
    return "LAN";
}

// --- methods ---

VideoOutcome sourceAdd(const ParamTree& t, const Args& args) {
    VideoOutcome o;
    const auto kind = strArg(args, "kind");
    const auto* spec = schemaV1().find("/video/source/1/kind");
    if (!kind || std::find(spec->choices.begin(), spec->choices.end(), *kind) == spec->choices.end()) {
        o.result = error(ErrorCode::OutOfRange, "kind must be display, window, camera, image, media, text, color or lan");
        return o;
    }
    const int id = firstFree([&](int n) { return sourceExists(t, n); }, kMaxVideoSources);
    if (id == 0) {
        o.result = error(ErrorCode::LimitReached, std::to_string(kMaxVideoSources) + " sources already exist");
        return o;
    }
    o.changes = videoSourceDefaults(id, *kind);
    if (auto name = strArg(args, "name")) put(o.changes, sourceBase(id) + "/name", *name);
    o.result.value = std::int64_t{id};
    return o;
}

VideoOutcome sourceRemove(const ParamTree& t, const Args& args) {
    VideoOutcome o;
    const int id = static_cast<int>(intArg(args, "source").value_or(0));
    if (!sourceExists(t, id)) {
        o.result = error(ErrorCode::NotFound, "no such source");
        return o;
    }
    o.removed.push_back(sourceBase(id));
    // layers showing the source go with it (as in OBS); the remaining layers are renumbered
    for (int scene : videoScenesByPosition(t)) {
        std::vector<int> keep;
        bool changed = false;
        for (int layer : videoLayersByPosition(t, scene)) {
            if (t.getAs<std::int64_t>(layerBase(scene, layer) + "/source").value_or(0) == id) {
                o.removed.push_back(layerBase(scene, layer));
                changed = true;
            } else {
                keep.push_back(layer);
            }
        }
        if (changed) renumber(o.changes, keep, [&](int l) { return layerBase(scene, l); });
    }
    return o;
}

VideoOutcome sourceReport(const ParamTree& t, const Args& args, ClientRole role) {
    VideoOutcome o;
    if (role != ClientRole::Service) {
        o.result = error(ErrorCode::Unauthorized, "only the capture process reports");
        return o;
    }
    const int id = static_cast<int>(intArg(args, "source").value_or(0));
    if (!sourceExists(t, id)) {
        o.result = error(ErrorCode::NotFound, "no such source");  // removed meanwhile; capd ignores this
        return o;
    }
    const auto state = strArg(args, "state");
    if (!state || (*state != "none" && *state != "ok" && *state != "disconnected" && *state != "error")) {
        o.result = error(ErrorCode::BadRequest, "state must be none, ok, disconnected or error");
        return o;
    }
    const std::string b = sourceBase(id);
    o.changes.push_back({b + "/state", *state});
    o.changes.push_back({b + "/error", *state == "error" ? strArg(args, "error").value_or("") : std::string()});
    if (auto w = intArg(args, "width")) o.changes.push_back({b + "/width", std::clamp<std::int64_t>(*w, 0, 16384)});
    if (auto h = intArg(args, "height")) o.changes.push_back({b + "/height", std::clamp<std::int64_t>(*h, 0, 16384)});
    return o;
}

VideoOutcome sceneAdd(const ParamTree& t, const Args& args) {
    VideoOutcome o;
    const int id = firstFree([&](int n) { return sceneExists(t, n); }, kMaxVideoScenes);
    if (id == 0) {
        o.result = error(ErrorCode::LimitReached, std::to_string(kMaxVideoScenes) + " scenes already exist");
        return o;
    }
    auto order = videoScenesByPosition(t);
    const auto count = static_cast<std::int64_t>(order.size());
    const auto position = intArg(args, "position").value_or(count + 1);
    if (position < 1 || position > count + 1) {
        o.result = error(ErrorCode::OutOfRange, "position out of range");
        return o;
    }
    order.insert(order.begin() + (position - 1), id);
    o.changes = videoSceneDefaults(id);
    if (auto name = strArg(args, "name")) put(o.changes, sceneBase(id) + "/name", *name);
    renumber(o.changes, order, sceneBase);
    o.result.value = std::int64_t{id};
    return o;
}

VideoOutcome sceneRemove(const ParamTree& t, const Args& args) {
    VideoOutcome o;
    const int id = static_cast<int>(intArg(args, "scene").value_or(0));
    if (!sceneExists(t, id)) {
        o.result = error(ErrorCode::NotFound, "no such scene");
        return o;
    }
    o.removed.push_back(sceneBase(id));
    auto order = videoScenesByPosition(t);
    order.erase(std::remove(order.begin(), order.end(), id), order.end());
    renumber(o.changes, order, sceneBase);
    for (int mix : videoMixes(t))
        for (const char* key : {"/program_scene", "/preview_scene"})
            if (t.getAs<std::int64_t>(mixBase(mix) + key).value_or(0) == id) o.changes.push_back({mixBase(mix) + key, std::int64_t{0}});
    return o;
}

VideoOutcome sceneMove(const ParamTree& t, const Args& args) {
    VideoOutcome o;
    const int id = static_cast<int>(intArg(args, "scene").value_or(0));
    if (!sceneExists(t, id)) {
        o.result = error(ErrorCode::NotFound, "no such scene");
        return o;
    }
    auto order = videoScenesByPosition(t);
    const auto position = intArg(args, "position").value_or(0);
    if (position < 1 || position > static_cast<std::int64_t>(order.size())) {
        o.result = error(ErrorCode::OutOfRange, "position out of range");
        return o;
    }
    order.erase(std::remove(order.begin(), order.end(), id), order.end());
    order.insert(order.begin() + (position - 1), id);
    renumber(o.changes, order, sceneBase);
    return o;
}

// scene number from "/video/scene/<n>/layer/..."
int sceneOfMethod(const std::string& method) {
    const std::string prefix = "/video/scene/";
    return std::atoi(method.c_str() + prefix.size());
}

VideoOutcome layerAdd(const ParamTree& t, int scene, const Args& args) {
    VideoOutcome o;
    if (!sceneExists(t, scene)) {
        o.result = error(ErrorCode::NotFound, "no such scene");
        return o;
    }
    const int source = static_cast<int>(intArg(args, "source").value_or(0));
    if (source != 0 && !sourceExists(t, source)) {
        o.result = error(ErrorCode::NotFound, "no such source");
        return o;
    }
    const int id = firstFree([&](int n) { return layerExists(t, scene, n); }, kMaxLayersPerScene);
    if (id == 0) {
        o.result = error(ErrorCode::LimitReached, std::to_string(kMaxLayersPerScene) + " layers already exist in the scene");
        return o;
    }
    auto order = videoLayersByPosition(t, scene);
    const auto count = static_cast<std::int64_t>(order.size());
    const auto position = intArg(args, "position").value_or(1);
    if (position < 1 || position > count + 1) {
        o.result = error(ErrorCode::OutOfRange, "position out of range");
        return o;
    }
    order.insert(order.begin() + (position - 1), id);
    o.changes = videoLayerDefaults(scene, id, source);
    if (auto name = strArg(args, "name")) put(o.changes, layerBase(scene, id) + "/name", *name);
    renumber(o.changes, order, [&](int l) { return layerBase(scene, l); });
    o.result.value = std::int64_t{id};
    return o;
}

VideoOutcome layerRemove(const ParamTree& t, int scene, const Args& args) {
    VideoOutcome o;
    const int id = static_cast<int>(intArg(args, "layer").value_or(0));
    if (!sceneExists(t, scene) || !layerExists(t, scene, id)) {
        o.result = error(ErrorCode::NotFound, "no such layer");
        return o;
    }
    o.removed.push_back(layerBase(scene, id));
    auto order = videoLayersByPosition(t, scene);
    order.erase(std::remove(order.begin(), order.end(), id), order.end());
    renumber(o.changes, order, [&](int l) { return layerBase(scene, l); });
    return o;
}

VideoOutcome layerMove(const ParamTree& t, int scene, const Args& args) {
    VideoOutcome o;
    const int id = static_cast<int>(intArg(args, "layer").value_or(0));
    if (!sceneExists(t, scene) || !layerExists(t, scene, id)) {
        o.result = error(ErrorCode::NotFound, "no such layer");
        return o;
    }
    auto order = videoLayersByPosition(t, scene);
    const auto position = intArg(args, "position").value_or(0);
    if (position < 1 || position > static_cast<std::int64_t>(order.size())) {
        o.result = error(ErrorCode::OutOfRange, "position out of range");
        return o;
    }
    order.erase(std::remove(order.begin(), order.end(), id), order.end());
    order.insert(order.begin() + (position - 1), id);
    renumber(o.changes, order, [&](int l) { return layerBase(scene, l); });
    return o;
}

VideoOutcome mixAdd(const ParamTree& t, const Args& args) {
    VideoOutcome o;
    const int id = firstFree([&](int n) { return mixExists(t, n); }, kMaxVideoMixes);
    if (id == 0) {
        o.result = error(ErrorCode::LimitReached, std::to_string(kMaxVideoMixes) + " video mixes already exist");
        return o;
    }
    o.changes = videoMixDefaults(id);
    if (auto name = strArg(args, "name")) put(o.changes, mixBase(id) + "/name", *name);
    o.result.value = std::int64_t{id};
    return o;
}

VideoOutcome mixRemove(const ParamTree& t, const Args& args) {
    VideoOutcome o;
    const int id = static_cast<int>(intArg(args, "mix").value_or(0));
    if (!mixExists(t, id)) {
        o.result = error(ErrorCode::NotFound, "no such video mix");
        return o;
    }
    o.removed.push_back(mixBase(id));
    for (const char* ref : kMixRefs)
        if (t.getAs<std::int64_t>(ref).value_or(0) == id) o.changes.push_back({ref, std::int64_t{0}});
    return o;
}

VideoOutcome mixTake(const ParamTree& t, const std::string& method) {
    VideoOutcome o;
    const int id = std::atoi(method.c_str() + std::string("/video/mix/").size());
    if (!mixExists(t, id)) {
        o.result = error(ErrorCode::NotFound, "no such video mix");
        return o;
    }
    const auto program = t.getAs<std::int64_t>(mixBase(id) + "/program_scene").value_or(0);
    const auto preview = t.getAs<std::int64_t>(mixBase(id) + "/preview_scene").value_or(0);
    // capd animates whenever program_scene changes, using the mix's transition settings
    o.changes.push_back({mixBase(id) + "/program_scene", preview});
    o.changes.push_back({mixBase(id) + "/preview_scene", program});
    return o;
}

VideoOutcome devicesReport(const Args& args, ClientRole role) {
    VideoOutcome o;
    if (role != ClientRole::Service) {
        o.result = error(ErrorCode::Unauthorized, "only the capture process reports");
        return o;
    }
    const auto kind = strArg(args, "kind");
    if (!kind || (*kind != "display" && *kind != "window" && *kind != "camera")) {
        o.result = error(ErrorCode::BadRequest, "kind must be display, window or camera");
        return o;
    }
    const std::string base = "/video/devices/" + *kind + "/";
    o.removed.push_back("/video/devices/" + *kind);
    for (const auto& [key, value] : args) {
        if (key == "kind") continue;
        const std::string path = base + key;
        const auto* spec = isValidPath(path) ? schemaV1().find(path) : nullptr;
        if (!spec || schemaV1().validate(*spec, value, false)) {
            o.result = error(ErrorCode::BadRequest, "bad entry " + key);
            o.removed.clear();
            o.changes.clear();
            return o;
        }
        o.changes.push_back({path, Schema::normalize(*spec, value)});
    }
    return o;
}

} // namespace

const std::vector<std::string>& videoMethodPatterns() {
    static const std::vector<std::string> patterns = {
        "/video/source/add",       "/video/source/remove",        "/video/source/report",
        "/video/scene/add",        "/video/scene/remove",         "/video/scene/move",
        "/video/scene/#/layer/add", "/video/scene/#/layer/remove", "/video/scene/#/layer/move",
        "/video/mix/add",          "/video/mix/remove",           "/video/mix/#/take",
        "/video/devices/refresh",  "/video/devices/report",
    };
    return patterns;
}

VideoOutcome videoCall(const ParamTree& tree, const std::string& method, const Args& args, ClientRole role) {
    const std::string pattern = Schema::patternOf(method);
    if (pattern == "/video/source/add") return sourceAdd(tree, args);
    if (pattern == "/video/source/remove") return sourceRemove(tree, args);
    if (pattern == "/video/source/report") return sourceReport(tree, args, role);
    if (pattern == "/video/scene/add") return sceneAdd(tree, args);
    if (pattern == "/video/scene/remove") return sceneRemove(tree, args);
    if (pattern == "/video/scene/move") return sceneMove(tree, args);
    if (pattern == "/video/scene/#/layer/add") return layerAdd(tree, sceneOfMethod(method), args);
    if (pattern == "/video/scene/#/layer/remove") return layerRemove(tree, sceneOfMethod(method), args);
    if (pattern == "/video/scene/#/layer/move") return layerMove(tree, sceneOfMethod(method), args);
    if (pattern == "/video/mix/add") return mixAdd(tree, args);
    if (pattern == "/video/mix/remove") return mixRemove(tree, args);
    if (pattern == "/video/mix/#/take") return mixTake(tree, method);
    if (pattern == "/video/devices/report") return devicesReport(args, role);
    VideoOutcome o;
    if (pattern == "/video/devices/refresh") {
        o.event.name = "video.devices.refresh";
        return o;
    }
    o.result = error(ErrorCode::UnknownMethod, "unknown video method " + method);
    return o;
}

std::vector<Change> videoSourceDefaults(int source, const std::string& kind, const Schema& schema) {
    auto c = expandFixed(schema, "/video/source/#/", {source});
    put(c, sourceBase(source) + "/kind", kind);
    put(c, sourceBase(source) + "/name", std::string(defaultSourceName(kind)) + " " + std::to_string(source));
    return c;
}

std::vector<Change> videoSceneDefaults(int scene, const Schema& schema) {
    auto c = expandFixed(schema, "/video/scene/#/", {scene});
    put(c, sceneBase(scene) + "/name", "Scene " + std::to_string(scene));
    return c;
}

std::vector<Change> videoLayerDefaults(int scene, int layer, int source, const Schema& schema) {
    auto c = expandFixed(schema, "/video/scene/#/layer/#/", {scene, layer});
    put(c, layerBase(scene, layer) + "/source", std::int64_t{source});
    return c;
}

std::vector<Change> videoMixDefaults(int mix, const Schema& schema) {
    auto c = expandFixed(schema, "/video/mix/#/", {mix});
    put(c, mixBase(mix) + "/name", std::string("Mix ") + static_cast<char>('A' + (mix - 1) % 26));
    return c;
}

std::vector<Change> videoStartDefaults(const Schema& schema) {
    auto c = videoSourceDefaults(1, "display", schema);
    for (auto& x : videoSceneDefaults(1, schema)) c.push_back(std::move(x));
    put(c, sceneBase(1) + "/position", std::int64_t{1});
    for (auto& x : videoLayerDefaults(1, 1, 1, schema)) c.push_back(std::move(x));
    put(c, layerBase(1, 1) + "/position", std::int64_t{1});
    return c;
}

ValidationError validateVideo(const ParamTree& after, const std::vector<Change>& changes) {
    auto fail = [](std::string message) { return ValidationError{ErrorCode::NotFound, std::move(message)}; };
    for (const auto& c : changes) {
        const auto* n = std::get_if<std::int64_t>(&c.value);
        if (!n || *n == 0) continue;
        const std::string pattern = Schema::patternOf(c.path);
        if (pattern == "/video/scene/#/layer/#/source" && !sourceExists(after, static_cast<int>(*n)))
            return fail("no such source: " + std::to_string(*n));
        if ((pattern == "/video/mix/#/program_scene" || pattern == "/video/mix/#/preview_scene") &&
            !sceneExists(after, static_cast<int>(*n)))
            return fail("no such scene: " + std::to_string(*n));
        for (const char* ref : kMixRefs)
            if (c.path == ref && !mixExists(after, static_cast<int>(*n))) return fail("no such video mix: " + std::to_string(*n));
    }
    return {};
}

std::vector<Change> videoDerived(const ParamTree& tree) {
    std::vector<Change> out;
    for (int mix : videoMixes(tree)) {
        bool active = false;
        for (const char* ref : kMixRefs) active = active || tree.getAs<std::int64_t>(ref).value_or(0) == mix;
        out.push_back({mixBase(mix) + "/active", active});
    }
    return out;
}

std::vector<int> videoScenesByPosition(const ParamTree& tree) {
    return byPosition(tree, numbersUnder(tree, "/video/scene/", "/position"), sceneBase);
}

std::vector<int> videoLayersByPosition(const ParamTree& tree, int scene) {
    return byPosition(tree, numbersUnder(tree, sceneBase(scene) + "/layer/", "/position"),
                      [&](int l) { return layerBase(scene, l); });
}

std::vector<int> videoSources(const ParamTree& tree) { return numbersUnder(tree, "/video/source/", "/kind"); }

std::vector<int> videoMixes(const ParamTree& tree) { return numbersUnder(tree, "/video/mix/", "/width"); }

} // namespace dc::proto
