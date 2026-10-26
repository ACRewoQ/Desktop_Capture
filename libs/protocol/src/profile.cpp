#include "dc/protocol/profile.hpp"

#include "dc/protocol/defaults.hpp"
#include "dc/protocol/fx.hpp"
#include "dc/protocol/param_store.hpp"
#include "dc/protocol/video.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <set>
#include <string>

namespace dc::proto {

namespace {

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }
bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool isStripPattern(std::string_view p) { return startsWith(p, "/track/#/") || startsWith(p, "/master/"); }

std::optional<int> toInt(std::string_view s) {
    int v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

// "/track/12/..." -> 12 for prefix "/track/"; nullopt when the next segment is not a number.
std::optional<int> indexAfter(std::string_view path, std::string_view prefix) {
    if (!startsWith(path, prefix)) return std::nullopt;
    const auto rest = path.substr(prefix.size());
    return toInt(rest.substr(0, rest.find('/')));
}

std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

std::string unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 == s.size()) {
            out += s[i];
            continue;
        }
        const char n = s[++i];
        out += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
    }
    return out;
}

std::vector<std::string_view> splitTabs(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;) {
        const auto tab = line.find('\t', start);
        out.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
        if (tab == std::string_view::npos) return out;
        start = tab + 1;
    }
}

// type letter and text of a value
void appendValue(std::string& out, const Value& v) {
    switch (typeOf(v)) {
        case ValueType::Nil: out += "n\t"; break;
        case ValueType::Bool: out += std::get<bool>(v) ? "b\t1" : "b\t0"; break;
        case ValueType::Int: out += "i\t" + std::to_string(std::get<std::int64_t>(v)); break;
        case ValueType::Float: {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.17g", std::get<double>(v));
            out += "f\t";
            out += buf;
            break;
        }
        case ValueType::String: out += "s\t" + escape(std::get<std::string>(v)); break;
    }
}

std::optional<Value> parseValue(std::string_view type, std::string_view text) {
    if (type == "n") return Value{};
    if (type == "b") {
        if (text == "1") return Value{true};
        if (text == "0") return Value{false};
        return std::nullopt;
    }
    if (type == "i") {
        std::int64_t v = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
        if (ec != std::errc() || ptr != text.data() + text.size()) return std::nullopt;
        return Value{v};
    }
    if (type == "f") {
        // from_chars for double is not everywhere yet: strtod on a copy
        const std::string copy(text);
        char* end = nullptr;
        const double d = std::strtod(copy.c_str(), &end);
        if (copy.empty() || end != copy.c_str() + copy.size()) return std::nullopt;
        return Value{d};
    }
    if (type == "s") return Value{unescape(text)};
    return std::nullopt;
}

constexpr int kFormat = 1;

} // namespace

bool inProfile(const ParamSpec& spec) {
    const std::string_view p = spec.pattern;
    // read-only values that a restore needs to rebuild the structure
    static const char* const kStructure[] = {"/track/#/position", "/track/#/fx/#/type", "/master/fx/#/type",
                                             "/video/scene/#/position", "/video/scene/#/layer/#/position",
                                             "/video/source/#/kind"};
    for (const char* s : kStructure)
        if (p == s) return true;
    if (spec.access == Access::ReadOnly) return false;
    if (endsWith(p, "/fx/#/editor")) return false; // a plugin window is not a setting
    static const char* const kPrefixes[] = {"/system/", "/cue/", "/track/", "/master/", "/capture/", "/recorder/",
                                            "/sampler/", "/video/", "/lan/stream/"};
    for (const char* s : kPrefixes)
        if (startsWith(p, s)) return true;
    return false;
}

bool inSnapshot(const ParamSpec& spec) {
    if (!inProfile(spec) || spec.access == Access::ReadOnly) return false;
    const std::string_view p = spec.pattern;
    if (p == "/cue/volume_db") return true;
    if (!isStripPattern(p)) return false;
    const std::string_view rest = p.substr(startsWith(p, "/master/") ? 8 : 9); // after "/master/" or "/track/#/"
    static const char* const kExact[] = {"volume_db", "pan", "mute", "solo", "lowcut/enabled", "lowcut/freq_hz",
                                         "highcut/enabled", "highcut/freq_hz", "send/#/level_db", "send/#/pre_fader",
                                         "fx/#/bypass", "fx/#/param/*"};
    for (const char* s : kExact)
        if (rest == s) return true;
    return false;
}

bool isValidProfileName(std::string_view name) {
    if (name.empty()) return false;
    if (name.front() == ' ' || name.back() == ' ' || name.back() == '.' || name.front() == '.') return false;
    std::size_t chars = 0;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f) return false;
        if (std::string_view("\\/:*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos) return false;
        if ((c & 0xC0) != 0x80) ++chars; // count UTF-8 code points
    }
    if (chars > 64) return false;
    // names Windows reserves for devices, also with an extension ("nul.txt")
    std::string base(name.substr(0, name.find('.')));
    for (auto& c : base) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    static const char* const kReserved[] = {"con", "prn", "aux", "nul"};
    for (const char* r : kReserved)
        if (base == r) return false;
    if (base.size() == 4 && (startsWith(base, "com") || startsWith(base, "lpt")) && base[3] >= '1' && base[3] <= '9')
        return false;
    return true;
}

} // namespace dc::proto

namespace dc::proto {

std::string writeProfile(const ProfileData& p) {
    std::string out = "dcprofile\t" + std::to_string(kFormat) + "\n";
    for (const auto& c : p.values) {
        out += "v\t" + escape(c.path) + "\t";
        appendValue(out, c.value);
        out += "\n";
    }
    for (const auto& d : p.devices)
        out += std::string("dev\t") + (d.input ? "in" : "out") + "\t" + escape(d.id) + "\t" + escape(d.name) + "\n";
    for (const auto& [slot, state] : p.fxState) out += "fx\t" + escape(slot) + "\t" + base64Encode(state) + "\n";
    for (const auto& sn : p.snapshots) {
        const auto n = std::to_string(sn.number);
        out += "snap\t" + n + "\t" + std::to_string(sn.modified) + "\t" + escape(sn.name) + "\n";
        for (const auto& c : sn.values) {
            out += "sv\t" + n + "\t" + escape(c.path) + "\t";
            appendValue(out, c.value);
            out += "\n";
        }
    }
    out += "end\n";
    return out;
}

std::optional<ProfileData> readProfile(std::string_view text, std::string* error, int* skippedLines) {
    auto fail = [&](const char* why) -> std::optional<ProfileData> {
        if (error) *error = why;
        return std::nullopt;
    };
    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start < text.size();) {
        auto nl = text.find('\n', start);
        if (nl == std::string_view::npos) nl = text.size();
        auto line = text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) lines.push_back(line);
        start = nl + 1;
    }
    if (lines.empty()) return fail("空のファイル");
    const auto head = splitTabs(lines.front());
    if (head.size() < 2 || head[0] != "dcprofile") return fail("プロファイルのファイルではない");
    const auto format = toInt(head[1]);
    if (!format || *format < 1) return fail("プロファイルの形式が読めない");
    if (*format > kFormat) return fail("新しい版のエンジンで保存したプロファイル");
    if (lines.back() != "end") return fail("ファイルが途中で切れている");

    ProfileData p;
    int skipped = 0;
    std::map<int, std::size_t> snapIndex;
    for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
        const auto f = splitTabs(lines[i]);
        bool ok = false;
        if (f[0] == "v" && f.size() == 4) {
            if (auto v = parseValue(f[2], f[3])) {
                p.values.push_back({unescape(f[1]), std::move(*v)});
                ok = true;
            }
        } else if (f[0] == "dev" && f.size() == 4 && (f[1] == "in" || f[1] == "out")) {
            p.devices.push_back({unescape(f[2]), unescape(f[3]), f[1] == "in"});
            ok = true;
        } else if (f[0] == "fx" && f.size() == 3) {
            if (auto state = base64Decode(f[2])) {
                p.fxState[unescape(f[1])] = std::move(*state);
                ok = true;
            }
        } else if (f[0] == "snap" && f.size() == 4) {
            const auto n = toInt(f[1]);
            std::int64_t modified = 0;
            const auto [ptr, ec] = std::from_chars(f[2].data(), f[2].data() + f[2].size(), modified);
            if (n && *n >= 1 && *n <= kMaxSnapshots && ec == std::errc() && !snapIndex.count(*n)) {
                snapIndex[*n] = p.snapshots.size();
                p.snapshots.push_back({*n, unescape(f[3]), modified, {}});
                ok = true;
            }
        } else if (f[0] == "sv" && f.size() == 5) {
            const auto n = toInt(f[1]);
            auto v = parseValue(f[3], f[4]);
            if (n && snapIndex.count(*n) && v) {
                p.snapshots[snapIndex[*n]].values.push_back({unescape(f[2]), std::move(*v)});
                ok = true;
            }
        } else if (f[0] != "v" && f[0] != "dev" && f[0] != "fx" && f[0] != "snap" && f[0] != "sv") {
            ok = true; // a newer engine's item: ignored
        }
        if (!ok) ++skipped;
    }
    if (skippedLines) *skippedLines = skipped;
    return p;
}

std::vector<Change> profileValues(const ParamTree& tree, const Schema& schema) {
    std::vector<Change> out;
    for (const auto& [path, value] : tree.values()) {
        const ParamSpec* spec = schema.find(path);
        if (spec && inProfile(*spec)) out.push_back({path, value});
    }
    return out;
}

std::vector<Change> snapshotValues(const ParamTree& tree, const Schema& schema) {
    std::vector<Change> out;
    for (const auto& [path, value] : tree.values()) {
        const ParamSpec* spec = schema.find(path);
        if (!spec) continue;
        if (inSnapshot(*spec) || spec->pattern == "/track/#/fx/#/type" || spec->pattern == "/master/fx/#/type")
            out.push_back({path, value});
    }
    return out;
}

std::vector<std::string> deviceRefs(const ParamTree& tree) {
    std::set<std::string> ids;
    for (const auto& [path, value] : tree.values()) {
        const auto* s = std::get_if<std::string>(&value);
        if (!s || s->empty()) continue;
        if (path == "/system/clock_device") {
            ids.insert(*s);
        } else if (endsWith(path, "/input/source")) {
            const auto kind = tree.getAs<std::string>(path.substr(0, path.size() - 7) + "/kind");
            if (kind == "device") ids.insert(*s);
        } else if (endsWith(path, "/target")) {
            for (std::string_view prefix : {"device/", "vmic/"})
                if (startsWith(*s, prefix)) ids.insert(s->substr(prefix.size()));
        }
    }
    return {ids.begin(), ids.end()};
}

namespace {

struct FxSlotRef {
    int strip = -1; // 0 = master
    int slot = 0;
    std::string rest; // "type", "bypass", "param/ratio"
};
std::optional<FxSlotRef> parseFxSlot(std::string_view path) {
    FxSlotRef r;
    std::string_view rest;
    if (startsWith(path, "/master/fx/")) {
        r.strip = 0;
        rest = path.substr(11);
    } else if (const auto n = indexAfter(path, "/track/")) {
        const auto after = path.substr(7 + std::to_string(*n).size());
        if (!startsWith(after, "/fx/")) return std::nullopt;
        r.strip = *n;
        rest = after.substr(4);
    } else {
        return std::nullopt;
    }
    const auto slash = rest.find('/');
    const auto slot = toInt(rest.substr(0, slash));
    if (!slot || *slot < 1 || slash == std::string_view::npos) return std::nullopt;
    r.slot = *slot;
    r.rest = std::string(rest.substr(slash + 1));
    return r;
}

std::string stripPath(int strip) { return strip == 0 ? std::string("/master") : "/track/" + std::to_string(strip); }

bool isVideoEntity(std::string_view p) {
    return startsWith(p, "/video/source/") || startsWith(p, "/video/scene/") || startsWith(p, "/video/mix/");
}

} // namespace

RestorePlan planRestore(const ParamTree& current, const std::vector<Change>& values, const Schema& schema) {
    RestorePlan plan;
    plan.removed = {"/track", "/master/fx", "/video/source", "/video/scene", "/video/mix"};
    ParamTree work = current;
    for (const auto& r : plan.removed) work.removeSubtree(r);
    std::map<std::string, Value> out; // what the plan sets, by path
    auto put = [&](const Change& c) {
        work.set(c.path, c.value);
        out[c.path] = c.value;
    };

    // the file's values, checked against the schema (last one wins)
    std::map<std::string, Value> file;
    for (const auto& c : values) {
        const ParamSpec* spec = isValidPath(c.path) ? schema.find(c.path) : nullptr;
        if (!spec || !inProfile(*spec) || schema.validate(*spec, c.value, false)) {
            ++plan.skipped;
            continue;
        }
        file[c.path] = Schema::normalize(*spec, c.value);
    }
    auto fileInt = [&](const std::string& path) -> std::optional<std::int64_t> {
        if (auto it = file.find(path); it != file.end())
            if (auto* v = std::get_if<std::int64_t>(&it->second)) return *v;
        return std::nullopt;
    };

    // defaults of everything the profile keeps outside the numbered entities
    for (const auto& c : globalDefaults(schema)) {
        const ParamSpec* spec = schema.find(c.path);
        if (spec && inProfile(*spec) && !isVideoEntity(c.path)) put(c);
    }
    for (const auto& c : masterDefaults(schema)) {
        const ParamSpec* spec = schema.find(c.path);
        if (spec && inProfile(*spec)) put(c);
    }

    // numbered entities, ordered by their saved position (then number) and renumbered 1..n
    auto ordered = [&](std::vector<std::pair<std::int64_t, int>> v) {
        std::sort(v.begin(), v.end());
        std::vector<int> numbers;
        for (const auto& [pos, n] : v) numbers.push_back(n);
        return numbers;
    };
    std::vector<std::pair<std::int64_t, int>> tracks, sources, scenes;
    std::set<int> mixes;
    std::map<int, std::vector<std::pair<std::int64_t, int>>> layers;
    for (const auto& [path, value] : file) {
        if (const auto n = indexAfter(path, "/track/"); n && path == "/track/" + std::to_string(*n) + "/position") {
            if (*n >= 1 && *n <= kMaxTracks) tracks.push_back({*fileInt(path), *n});
        } else if (const auto src = indexAfter(path, "/video/source/");
                   src && path == "/video/source/" + std::to_string(*src) + "/kind") {
            if (*src >= 1 && *src <= kMaxVideoSources) sources.push_back({0, *src});
        } else if (const auto sc = indexAfter(path, "/video/scene/")) {
            const auto base = "/video/scene/" + std::to_string(*sc);
            if (path == base + "/position" && *sc >= 1 && *sc <= kMaxVideoScenes) scenes.push_back({*fileInt(path), *sc});
            const auto l = indexAfter(path, base + "/layer/");
            if (l && path == base + "/layer/" + std::to_string(*l) + "/position" && *l >= 1 && *l <= kMaxLayersPerScene)
                layers[*sc].push_back({*fileInt(path), *l});
        } else if (const auto m = indexAfter(path, "/video/mix/")) {
            if (*m >= 1 && *m <= kMaxVideoMixes) mixes.insert(*m);
        }
    }
    int position = 0;
    const auto trackOrder = ordered(tracks);
    for (int n : trackOrder) {
        for (const auto& c : trackDefaults(n, schema)) put(c);
        put({"/track/" + std::to_string(n) + "/position", std::int64_t{++position}});
    }
    put({"/mixer/track_count", static_cast<std::int64_t>(trackOrder.size())});
    for (int n : ordered(sources)) {
        const auto& kind = std::get<std::string>(file["/video/source/" + std::to_string(n) + "/kind"]);
        for (const auto& c : videoSourceDefaults(n, kind, schema)) put(c);
    }
    std::set<int> sceneSet;
    position = 0;
    for (int n : ordered(scenes)) {
        sceneSet.insert(n);
        for (const auto& c : videoSceneDefaults(n, schema)) put(c);
        put({"/video/scene/" + std::to_string(n) + "/position", std::int64_t{++position}});
    }
    for (auto& [scene, list] : layers) {
        if (!sceneSet.count(scene)) continue;
        int layerPos = 0;
        for (int l : ordered(list)) {
            const auto base = "/video/scene/" + std::to_string(scene) + "/layer/" + std::to_string(l);
            // the source comes with the checked values below
            for (const auto& c : videoLayerDefaults(scene, l, 0, schema)) put(c);
            put({base + "/position", std::int64_t{++layerPos}});
        }
    }
    for (int m : mixes)
        for (const auto& c : videoMixDefaults(m, schema)) put(c);

    // effect slots of existing strips, in slot order (at most 16)
    std::map<std::pair<int, int>, FxRestore> slots;
    for (const auto& [path, value] : file) {
        const auto ref = parseFxSlot(path);
        if (!ref) continue;
        if (ref->strip > 0 && !work.contains(stripPath(ref->strip) + "/position")) continue; // counted below
        auto& s = slots[{ref->strip, ref->slot}];
        s.fileSlot = stripPath(ref->strip) + "/fx/" + std::to_string(ref->slot);
        if (ref->rest == "type") s.type = std::get<std::string>(value);
        else s.values.push_back({"/" + ref->rest, value});
    }
    for (auto& [key, s] : slots) {
        auto& list = plan.fx[key.first];
        const FxTypeSpec* builtin = findBuiltinFx(s.type);
        if ((!builtin && !isVst3Type(s.type)) || list.size() >= static_cast<std::size_t>(kMaxFxPerTrack)) {
            plan.skipped += static_cast<int>(s.values.size()) + 1;
            continue;
        }
        std::vector<std::pair<std::string, Value>> kept;
        for (auto& [suffix, v] : s.values) {
            bool ok = suffix == "/bypass";
            if (startsWith(suffix, "/param/")) {
                const auto d = asNumber(v);
                if (builtin) {
                    const FxParamSpec* p = findFxParam(*builtin, suffix.substr(7));
                    ok = p && d && *d >= p->min && *d <= p->max;
                } else {
                    ok = d && *d >= 0.0 && *d <= 1.0;
                }
            }
            if (ok) kept.push_back({suffix, v});
            else ++plan.skipped;
        }
        s.values = std::move(kept);
        list.push_back(std::move(s));
    }
    for (auto it = plan.fx.begin(); it != plan.fx.end();)
        it = it->second.empty() ? plan.fx.erase(it) : std::next(it);

    // the remaining values, each checked like a client's Set against what is already in place
    for (const auto& [path, value] : file) {
        if (parseFxSlot(path)) {
            if (const auto ref = parseFxSlot(path); ref->strip > 0 && !work.contains(stripPath(ref->strip) + "/position"))
                ++plan.skipped;
            continue;
        }
        if (!work.contains(path)) { // belongs to an entity that is not there (a track without its position)
            ++plan.skipped;
            continue;
        }
        const ParamSpec* spec = schema.find(path);
        if (spec->access == Access::ReadOnly) continue; // structure, already in place
        const Value before = *work.get(path);
        work.set(path, value);
        if (defaultSemanticValidator(work, {{path, value}})) {
            work.set(path, before);
            ++plan.skipped;
            continue;
        }
        out[path] = value;
    }

    plan.changes.reserve(out.size());
    for (auto& [path, value] : out) plan.changes.push_back({path, std::move(value)});
    return plan;
}

std::vector<Change> planRecall(const ParamTree& tree, const std::vector<Change>& snapshot) {
    std::map<std::string, std::string> types; // slot base -> type when the snapshot was taken
    for (const auto& c : snapshot)
        if (const auto ref = parseFxSlot(c.path); ref && ref->rest == "type")
            if (const auto* t = std::get_if<std::string>(&c.value))
                types[stripPath(ref->strip) + "/fx/" + std::to_string(ref->slot)] = *t;
    std::vector<Change> out;
    for (const auto& c : snapshot) {
        if (!tree.contains(c.path)) continue;
        if (const auto ref = parseFxSlot(c.path)) {
            if (ref->rest == "type") continue;
            const auto slot = stripPath(ref->strip) + "/fx/" + std::to_string(ref->slot);
            const auto it = types.find(slot);
            if (it == types.end() || tree.getAs<std::string>(slot + "/type") != it->second) continue;
        }
        if (*tree.get(c.path) != c.value) out.push_back(c);
    }
    return out;
}

std::string base64Encode(std::string_view data) {
    static const char* const kDigits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t b0 = static_cast<unsigned char>(data[i]);
        const std::uint32_t b1 = i + 1 < data.size() ? static_cast<unsigned char>(data[i + 1]) : 0;
        const std::uint32_t b2 = i + 2 < data.size() ? static_cast<unsigned char>(data[i + 2]) : 0;
        const std::uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out += kDigits[(v >> 18) & 63];
        out += kDigits[(v >> 12) & 63];
        out += i + 1 < data.size() ? kDigits[(v >> 6) & 63] : '=';
        out += i + 2 < data.size() ? kDigits[v & 63] : '=';
    }
    return out;
}

std::optional<std::string> base64Decode(std::string_view text) {
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    if (text.size() % 4 != 0) return std::nullopt;
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        int d[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + static_cast<std::size_t>(k)];
            if (c == '=' && i + 4 == text.size() && k >= 2) {
                d[k] = 0;
                ++pad;
            } else if (pad > 0 || (d[k] = digit(c)) < 0) {
                return std::nullopt;
            }
        }
        const std::uint32_t v = (static_cast<std::uint32_t>(d[0]) << 18) | (static_cast<std::uint32_t>(d[1]) << 12) |
                                (static_cast<std::uint32_t>(d[2]) << 6) | static_cast<std::uint32_t>(d[3]);
        out += static_cast<char>((v >> 16) & 0xFF);
        if (pad < 2) out += static_cast<char>((v >> 8) & 0xFF);
        if (pad < 1) out += static_cast<char>(v & 0xFF);
    }
    return out;
}

} // namespace dc::proto
