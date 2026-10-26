#include "dc/protocol/control.hpp"

#include "dc/protocol/fx.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace dc::proto::ctl {

namespace {

struct KeyDef {
    std::string name;
    int vk;
};

std::vector<KeyDef> buildKeys() {
    std::vector<KeyDef> k;
    auto add = [&](std::string name, int vk) { k.push_back({std::move(name), vk}); };
    for (char c = 'a'; c <= 'z'; ++c) add(std::string(1, c), 'A' + (c - 'a'));
    for (char c = '0'; c <= '9'; ++c) add(std::string(1, c), c);
    for (int i = 1; i <= 24; ++i) add("f" + std::to_string(i), 0x70 + i - 1);
    for (int i = 0; i <= 9; ++i) add("num" + std::to_string(i), 0x60 + i);
    const KeyDef fixed[] = {
        {"num_add", 0x6B}, {"num_sub", 0x6D}, {"num_mul", 0x6A}, {"num_div", 0x6F}, {"num_dec", 0x6E},
        {"space", 0x20}, {"enter", 0x0D}, {"tab", 0x09}, {"backspace", 0x08}, {"escape", 0x1B},
        {"insert", 0x2D}, {"delete", 0x2E}, {"home", 0x24}, {"end", 0x23}, {"pageup", 0x21}, {"pagedown", 0x22},
        {"up", 0x26}, {"down", 0x28}, {"left", 0x25}, {"right", 0x27}, {"pause", 0x13}, {"printscreen", 0x2C},
        {"scrolllock", 0x91}, {"minus", 0xBD}, {"equal", 0xBB}, {"comma", 0xBC}, {"period", 0xBE},
        {"semicolon", 0xBA}, {"slash", 0xBF}, {"backslash", 0xDC}, {"quote", 0xDE}, {"bracketleft", 0xDB},
        {"bracketright", 0xDD}, {"backquote", 0xC0}, {"media_play", 0xB3}, {"media_stop", 0xB2},
        {"media_next", 0xB0}, {"media_prev", 0xB1},
    };
    for (const auto& f : fixed) k.push_back(f);
    return k;
}

const std::vector<KeyDef>& keys() {
    static const std::vector<KeyDef> k = buildKeys();
    return k;
}

const char* const kModNames[] = {"ctrl", "alt", "shift", "win"};

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const auto p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string_view::npos ? std::string_view::npos : p - start));
        if (p == std::string_view::npos) break;
        start = p + 1;
    }
    return out;
}

std::optional<int> toInt(std::string_view s) {
    if (s.empty() || (s.size() > 1 && s[0] == '0')) return std::nullopt;
    int v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return v;
}

std::optional<double> toDouble(std::string_view s) {
    // from_chars for double is missing in some standard libraries; strtod on a copy is enough here
    if (s.empty()) return std::nullopt;
    const std::string copy(s);
    char* end = nullptr;
    const double v = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}

std::optional<Trigger> fail(std::string* error, const char* why) {
    if (error) *error = why;
    return std::nullopt;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool isVolumePath(std::string_view path) { return endsWith(path, "/volume_db") || endsWith(path, "/level_db"); }

// a writable parameter the bindings may change: in the schema, read-write, present, not /control
const ParamSpec* writable(const ParamTree& tree, std::string_view path, const Schema& schema, std::string* why) {
    if (!isValidPath(path)) {
        *why = "対象のパスが正しくない";
        return nullptr;
    }
    if (path.rfind("/control/", 0) == 0) {
        *why = "/control は対象にできない";
        return nullptr;
    }
    const ParamSpec* spec = schema.find(path);
    if (!spec) {
        *why = "対象のパラメーターがない";
        return nullptr;
    }
    if (spec->access == Access::ReadOnly) {
        *why = "対象が読み取り専用";
        return nullptr;
    }
    if (!tree.contains(path)) {
        *why = "対象がまだない（トラックやエフェクトがない）";
        return nullptr;
    }
    return spec;
}

double clampD(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

Value numberValue(const Range& r, double v) {
    v = clampD(v, r.min, r.max);
    if (r.integer) return static_cast<std::int64_t>(std::llround(v));
    return v;
}

} // namespace

std::optional<Trigger> parseTrigger(std::string_view text, std::string* error) {
    Trigger t;
    if (text.empty()) return t;
    if (text.rfind("key:", 0) == 0) {
        const auto parts = split(text.substr(4), '+');
        t.type = Trigger::Type::Key;
        int nextMod = 0;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            int m = nextMod;
            while (m < 4 && parts[i] != kModNames[m]) ++m;
            if (m == 4) return fail(error, "修飾キーは ctrl・alt・shift・win をこの順で");
            t.mods = static_cast<std::uint8_t>(t.mods | (1 << m));
            nextMod = m + 1;
        }
        if (virtualKeyOf(parts.back()) == 0) return fail(error, "知らないキーの名前");
        t.key = std::string(parts.back());
        return t;
    }
    if (text.rfind("midi:", 0) == 0) {
        const auto parts = split(text.substr(5), '/');
        t.type = Trigger::Type::Midi;
        const std::string_view kind = parts[0];
        if (kind == "note") t.midi = MidiKind::Note;
        else if (kind == "cc") t.midi = MidiKind::Cc;
        else if (kind == "pc") t.midi = MidiKind::Pc;
        else if (kind == "pb") t.midi = MidiKind::Pb;
        else return fail(error, "MIDI の種類は note・cc・pc・pb");
        const std::size_t want = t.midi == MidiKind::Pb ? 2 : 3;
        if (parts.size() != want) return fail(error, "MIDI のきっかけの形が正しくない");
        const auto ch = toInt(parts[1]);
        if (!ch || *ch < 1 || *ch > 16) return fail(error, "MIDI のチャンネルは 1〜16");
        t.channel = *ch;
        if (want == 3) {
            const auto n = toInt(parts[2]);
            if (!n || *n < 0 || *n > 127) return fail(error, "MIDI の番号は 0〜127");
            t.number = *n;
        }
        return t;
    }
    return fail(error, "きっかけは key: か midi: で始める");
}

std::string formatTrigger(const Trigger& t) {
    switch (t.type) {
    case Trigger::Type::None: return {};
    case Trigger::Type::Key: {
        std::string s = "key:";
        for (int m = 0; m < 4; ++m)
            if (t.mods & (1 << m)) s += std::string(kModNames[m]) + "+";
        return s + t.key;
    }
    case Trigger::Type::Midi: {
        static const char* const kinds[] = {"note", "cc", "pc", "pb"};
        std::string s = std::string("midi:") + kinds[static_cast<int>(t.midi)] + "/" + std::to_string(t.channel);
        if (t.midi != MidiKind::Pb) s += "/" + std::to_string(t.number);
        return s;
    }
    }
    return {};
}

const std::vector<std::string>& keyNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const auto& k : keys()) n.push_back(k.name);
        return n;
    }();
    return names;
}

int virtualKeyOf(std::string_view name) {
    for (const auto& k : keys())
        if (name == k.name) return k.vk;
    return 0;
}

std::string keyNameOf(int virtualKey) {
    for (const auto& k : keys())
        if (k.vk == virtualKey) return k.name;
    return {};
}

std::optional<MidiMessage> decodeMidi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    if (status < 0x80 || status >= 0xF0) return std::nullopt; // running status is resolved by the driver; system messages ignored
    MidiMessage m;
    m.channel = (status & 0x0F) + 1;
    const int d1 = data1 & 0x7F, d2 = data2 & 0x7F;
    switch (status & 0xF0) {
    case 0x90:
        m.kind = MidiKind::Note;
        m.number = d1;
        m.value = d2;
        m.press = d2 > 0;
        m.release = d2 == 0;
        return m;
    case 0x80:
        m.kind = MidiKind::Note;
        m.number = d1;
        m.value = 0;
        m.release = true;
        return m;
    case 0xB0:
        m.kind = MidiKind::Cc;
        m.number = d1;
        m.value = d2;
        m.press = d2 >= 64;
        m.release = d2 < 64;
        return m;
    case 0xC0:
        m.kind = MidiKind::Pc;
        m.number = d1;
        m.value = d1;
        m.press = true;
        return m;
    case 0xE0:
        m.kind = MidiKind::Pb;
        m.value = d1 | (d2 << 7);
        return m;
    default: return std::nullopt; // aftertouch
    }
}

Trigger triggerOf(const MidiMessage& m) {
    Trigger t;
    t.type = Trigger::Type::Midi;
    t.midi = m.kind;
    t.channel = m.channel;
    t.number = m.kind == MidiKind::Pb ? 0 : m.number;
    return t;
}

double normalizedValue(const MidiMessage& m) {
    switch (m.kind) {
    case MidiKind::Note: return m.press ? 1.0 : 0.0;
    case MidiKind::Cc: return m.value / 127.0;
    case MidiKind::Pc: return 1.0;
    case MidiKind::Pb: return m.value / 16383.0;
    }
    return 0.0;
}

Binding readBinding(const ParamTree& tree, int n) {
    const std::string base = "/control/binding/" + std::to_string(n) + "/";
    Binding b;
    b.trigger = tree.getAs<std::string>(base + "trigger").value_or("");
    b.device = tree.getAs<std::string>(base + "device").value_or("");
    b.action = tree.getAs<std::string>(base + "action").value_or("none");
    b.target = tree.getAs<std::string>(base + "target").value_or("");
    b.value = tree.getAs<std::string>(base + "value").value_or("");
    b.enabled = tree.getAs<bool>(base + "enabled").value_or(true);
    return b;
}

std::optional<PadRef> parsePad(std::string_view target) {
    const auto parts = split(target, '/');
    if (parts.size() > 2) return std::nullopt;
    PadRef r;
    const auto pad = toInt(parts.back());
    if (!pad || *pad < 1 || *pad > kSamplerPads) return std::nullopt;
    r.pad = *pad;
    if (parts.size() == 2) {
        const auto bank = toInt(parts[0]);
        if (!bank || *bank < 1 || *bank > kSamplerBanks) return std::nullopt;
        r.bank = *bank;
    }
    return r;
}

BindingCheck checkBinding(const ParamTree& tree, const Binding& b, const Schema& schema) {
    if (!b.enabled || b.trigger.empty() || b.action == "none") return {"off", {}};
    auto invalid = [](std::string why) { return BindingCheck{"invalid", std::move(why)}; };
    std::string why;
    const auto t = parseTrigger(b.trigger, &why);
    if (!t) return invalid(why);
    const bool isKey = t->type == Trigger::Type::Key;
    if (isKey && b.action == "param") return invalid("param は MIDI のフェーダー・つまみ用（キーには step か set を使う）");
    if (!isKey && t->midi == MidiKind::Pb && b.action != "param") return invalid("ピッチベンドは param だけ");

    const std::string& a = b.action;
    if (a == "record_toggle" || a == "record_start" || a == "record_stop" || a == "recorder_toggle") return {"ok", {}};
    if (a == "pad") {
        if (!parsePad(b.target)) return invalid("パッドは <パッド> か <バンク>/<パッド>");
        return {"ok", {}};
    }
    if (a == "pad_stop") {
        if (b.target.empty()) return {"ok", {}};
        const auto bank = toInt(b.target);
        if (!bank || *bank < 1 || *bank > kSamplerBanks) return invalid("バンクは 1〜8");
        return {"ok", {}};
    }
    if (a == "take") {
        const auto mix = toInt(b.target);
        if (!mix || *mix < 1 || *mix > kMaxVideoMixes) return invalid("ミックスは 1〜4");
        if (!tree.contains("/video/mix/" + b.target + "/name")) return invalid("その映像ミックスがない");
        return {"ok", {}};
    }
    if (a == "toggle" || a == "set" || a == "step" || a == "param") {
        const ParamSpec* spec = writable(tree, b.target, schema, &why);
        if (!spec) return invalid(why);
        if (a == "toggle" && spec->type != ValueType::Bool) return invalid("toggle の対象は オン・オフ のパラメーター");
        if (a == "set") {
            const auto v = parseValue(tree, b.target, b.value, schema);
            if (!v) return invalid("値がパラメーターに合わない");
        }
        if (a == "step") {
            if (!numberRange(tree, b.target, schema)) return invalid("step の対象は数値のパラメーター");
            const auto d = toDouble(b.value);
            if (!d || *d == 0.0) return invalid("増減の値（例 -1）がない");
        }
        if (a == "param" && !valueFromMidi(tree, b.target, 0.0, schema))
            return invalid("MIDI の値を当てはめられないパラメーター");
        return {"ok", {}};
    }
    return invalid("知らない動作");
}

std::optional<Range> numberRange(const ParamTree& tree, std::string_view path, const Schema& schema) {
    const ParamSpec* spec = schema.find(path);
    if (!spec) return std::nullopt;
    if (spec->type != ValueType::Float && spec->type != ValueType::Int) return std::nullopt;
    if (!spec->intChoices.empty()) return std::nullopt;
    if (const auto fx = parseFxParamPath(path)) {
        const auto type = tree.getAs<std::string>(fx->slot + "/type").value_or("");
        if (isVst3Type(type)) return Range{0.0, 1.0, false, false};
        const FxTypeSpec* ft = findBuiltinFx(type);
        const FxParamSpec* fp = ft ? findFxParam(*ft, fx->key) : nullptr;
        if (!fp) return std::nullopt;
        return Range{fp->min, fp->max, fp->log, fp->steps >= 2};
    }
    if (!spec->min || !spec->max) return std::nullopt;
    return Range{*spec->min, *spec->max, false, spec->type == ValueType::Int};
}

std::optional<Value> parseValue(const ParamTree& tree, std::string_view path, std::string_view text, const Schema& schema) {
    const ParamSpec* spec = schema.find(path);
    if (!spec) return std::nullopt;
    Value v;
    switch (spec->type) {
    case ValueType::Bool:
        if (text == "true" || text == "1" || text == "on") v = true;
        else if (text == "false" || text == "0" || text == "off") v = false;
        else return std::nullopt;
        break;
    case ValueType::Int: {
        const auto d = toDouble(text);
        if (!d || std::floor(*d) != *d) return std::nullopt;
        v = static_cast<std::int64_t>(*d);
        break;
    }
    case ValueType::Float: {
        const auto d = toDouble(text);
        if (!d) return std::nullopt;
        if (const auto r = numberRange(tree, path, schema); r && (*d < r->min || *d > r->max)) return std::nullopt;
        v = *d;
        break;
    }
    case ValueType::String: v = std::string(text); break;
    default: return std::nullopt;
    }
    if (schema.validate(*spec, v, false)) return std::nullopt;
    return Schema::normalize(*spec, std::move(v));
}

std::optional<Value> stepValue(const ParamTree& tree, std::string_view path, double delta, const Schema& schema) {
    const auto r = numberRange(tree, path, schema);
    if (!r) return std::nullopt;
    const Value* cur = tree.get(path);
    double now = 0;
    if (const auto* d = cur ? std::get_if<double>(cur) : nullptr) now = *d;
    else if (const auto* i = cur ? std::get_if<std::int64_t>(cur) : nullptr) now = static_cast<double>(*i);
    else return std::nullopt;
    // stepping a fader up from silence starts at the bottom of the fader scale
    if (isVolumePath(path) && now < kFaderMinDb) now = delta > 0 ? kFaderMinDb : now;
    double next = now + delta;
    if (isVolumePath(path) && next < kFaderMinDb) next = r->min;
    return numberValue(*r, next);
}

std::optional<Value> valueFromMidi(const ParamTree& tree, std::string_view path, double normalized, const Schema& schema) {
    const ParamSpec* spec = schema.find(path);
    if (!spec) return std::nullopt;
    const double n = clampD(normalized, 0.0, 1.0);
    if (spec->type == ValueType::Bool) return Value{n >= 0.5};
    auto pick = [&](std::size_t count) { return std::min(static_cast<std::size_t>(n * static_cast<double>(count)), count - 1); };
    if (spec->type == ValueType::String) {
        if (spec->choices.empty()) return std::nullopt;
        return Value{spec->choices[pick(spec->choices.size())]};
    }
    if (spec->type == ValueType::Int && !spec->intChoices.empty()) return Value{spec->intChoices[pick(spec->intChoices.size())]};
    const auto r = numberRange(tree, path, schema);
    if (!r) return std::nullopt;
    if (isVolumePath(path)) return numberValue(*r, faderToDb(n));
    if (r->log && r->min > 0) return numberValue(*r, r->min * std::pow(r->max / r->min, n));
    return numberValue(*r, r->min + n * (r->max - r->min));
}

double faderToDb(double position) {
    if (position <= 0.0) return kMinVolumeDb;
    return kFaderMinDb + std::min(position, 1.0) * (kMaxVolumeDb - kFaderMinDb);
}

double dbToFader(double db) {
    if (db <= kFaderMinDb) return 0.0;
    return std::min((db - kFaderMinDb) / (kMaxVolumeDb - kFaderMinDb), 1.0);
}

} // namespace dc::proto::ctl
