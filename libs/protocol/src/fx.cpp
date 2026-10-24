#include "dc/protocol/fx.hpp"

#include <charconv>

namespace dc::proto {

namespace {

FxParamSpec p(std::string key, std::string name, std::string unit, double lo, double hi, double def, bool log = false) {
    return {std::move(key), std::move(name), std::move(unit), lo, hi, def, 0, log};
}
FxParamSpec sw(std::string key, std::string name, bool def) {
    return {std::move(key), std::move(name), "", 0, 1, def ? 1.0 : 0.0, 2, false};
}

std::vector<FxTypeSpec> buildCatalog() {
    std::vector<FxTypeSpec> c;
    c.push_back({"eq4",
                 "4バンドEQ",
                 {
                     p("low_freq_hz", "Low 周波数", "Hz", 20, 1000, 100, true),
                     p("low_gain_db", "Low ゲイン", "dB", -18, 18, 0),
                     p("mid1_freq_hz", "Mid1 周波数", "Hz", 40, 10000, 400, true),
                     p("mid1_gain_db", "Mid1 ゲイン", "dB", -18, 18, 0),
                     p("mid1_q", "Mid1 Q", "", 0.1, 10, 1, true),
                     p("mid2_freq_hz", "Mid2 周波数", "Hz", 100, 16000, 2500, true),
                     p("mid2_gain_db", "Mid2 ゲイン", "dB", -18, 18, 0),
                     p("mid2_q", "Mid2 Q", "", 0.1, 10, 1, true),
                     p("high_freq_hz", "High 周波数", "Hz", 1000, 20000, 8000, true),
                     p("high_gain_db", "High ゲイン", "dB", -18, 18, 0),
                     p("output_db", "出力", "dB", -18, 18, 0),
                 },
                 0});
    c.push_back({"comp",
                 "コンプレッサー",
                 {
                     p("threshold_db", "スレッショルド", "dB", -60, 0, -18),
                     p("ratio", "レシオ", ":1", 1, 20, 4, true),
                     p("attack_ms", "アタック", "ms", 0.1, 200, 10, true),
                     p("release_ms", "リリース", "ms", 10, 2000, 150, true),
                     p("knee_db", "ニー", "dB", 0, 24, 6),
                     p("makeup_db", "メイクアップ", "dB", 0, 24, 0),
                 },
                 0});
    c.push_back({"gate",
                 "ゲート",
                 {
                     p("threshold_db", "スレッショルド", "dB", -90, 0, -50),
                     p("floor_db", "閉じたときの音量", "dB", -90, 0, -90),
                     p("attack_ms", "アタック", "ms", 0.1, 50, 0.5, true),
                     p("hold_ms", "ホールド", "ms", 0, 500, 50),
                     p("release_ms", "リリース", "ms", 5, 2000, 150, true),
                 },
                 0});
    c.push_back({"reverb",
                 "リバーブ",
                 {
                     p("size", "広さ", "", 0, 1, 0.5),
                     p("damping", "高域の減衰", "", 0, 1, 0.5),
                     p("predelay_ms", "プリディレイ", "ms", 0, 200, 10),
                     p("width", "ステレオ幅", "", 0, 1, 1),
                     p("mix", "ミックス", "", 0, 1, 0.25),
                 },
                 0});
    c.push_back({"delay",
                 "ディレイ",
                 {
                     p("time_ms", "ディレイタイム", "ms", 1, 2000, 300, true),
                     p("feedback", "フィードバック", "", 0, 0.95, 0.35),
                     p("highcut_hz", "フィードバックのハイカット", "Hz", 1000, 20000, 8000, true),
                     sw("ping_pong", "ピンポン", false),
                     p("mix", "ミックス", "", 0, 1, 0.25),
                 },
                 0});
    c.push_back({"limiter",
                 "リミッター",
                 {
                     p("input_db", "入力ゲイン", "dB", 0, 24, 0),
                     p("ceiling_db", "シーリング", "dB", -24, 0, -1),
                     p("release_ms", "リリース", "ms", 1, 1000, 50, true),
                 },
                 48}); // 1 ms look-ahead
    return c;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

const std::vector<FxTypeSpec>& builtinFx() {
    static const std::vector<FxTypeSpec> catalog = buildCatalog();
    return catalog;
}

const FxTypeSpec* findBuiltinFx(std::string_view type) {
    for (const auto& t : builtinFx())
        if (t.type == type) return &t;
    return nullptr;
}

const FxParamSpec* findFxParam(const FxTypeSpec& type, std::string_view key) {
    for (const auto& p : type.params)
        if (p.key == key) return &p;
    return nullptr;
}

std::string vst3ParamKey(std::uint32_t paramId) { return "p" + std::to_string(paramId); }

std::optional<std::uint32_t> vst3ParamId(std::string_view key) {
    if (key.size() < 2 || key[0] != 'p') return std::nullopt;
    std::uint32_t v = 0;
    auto [ptr, ec] = std::from_chars(key.data() + 1, key.data() + key.size(), v);
    if (ec != std::errc() || ptr != key.data() + key.size()) return std::nullopt;
    if (key.size() > 2 && key[1] == '0') return std::nullopt; // canonical form only
    return v;
}

std::optional<FxParamPath> parseFxParamPath(std::string_view path) {
    // /track/N/fx/M/param/KEY or /master/fx/M/param/KEY
    const auto paramPos = path.find("/param/");
    if (paramPos == std::string_view::npos) return std::nullopt;
    const auto slot = path.substr(0, paramPos);
    const auto key = path.substr(paramPos + 7);
    if (key.empty() || key.find('/') != std::string_view::npos) return std::nullopt;
    const auto pattern = Schema::patternOf(slot);
    if (pattern != "/track/#/fx/#" && pattern != "/master/fx/#") return std::nullopt;
    return FxParamPath{std::string(slot), std::string(key)};
}

ValidationError validateFxChanges(const ParamTree& after, const std::vector<std::string>& changedPaths) {
    for (const auto& path : changedPaths) {
        if (endsWith(path, "/editor")) {
            const auto slot = std::string_view(path).substr(0, path.size() - 7);
            const auto pattern = Schema::patternOf(slot);
            if (pattern != "/track/#/fx/#" && pattern != "/master/fx/#") continue;
            const auto type = after.getAs<std::string>(std::string(slot) + "/type").value_or("");
            if (after.getAs<bool>(path).value_or(false) && !isVst3Type(type))
                return {ErrorCode::Conflict, "only VST3 effects have a plugin window"};
            continue;
        }
        const auto fp = parseFxParamPath(path);
        if (!fp) continue;
        const auto* v = after.get(path);
        const auto value = v ? asNumber(*v) : std::nullopt;
        if (!value) continue; // the schema already checked the type
        const auto type = after.getAs<std::string>(fp->slot + "/type").value_or("");
        double lo = 0, hi = 1;
        if (const auto* t = findBuiltinFx(type)) {
            const auto* spec = findFxParam(*t, fp->key);
            if (!spec) return {ErrorCode::NotFound, "\"" + type + "\" has no parameter " + fp->key};
            lo = spec->min;
            hi = spec->max;
        } else if (!isVst3Type(type)) {
            return {ErrorCode::NotFound, "no effect in " + fp->slot};
        }
        if (*value < lo || *value > hi)
            return {ErrorCode::OutOfRange, path + " = " + toDisplayString(*v) + " is outside the allowed range"};
    }
    return {};
}

} // namespace dc::proto
