#include "dc/protocol/profile.hpp"

#include <string>

namespace dc::proto {

namespace {

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }
bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool isStripPattern(std::string_view p) { return startsWith(p, "/track/#/") || startsWith(p, "/master/"); }

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
