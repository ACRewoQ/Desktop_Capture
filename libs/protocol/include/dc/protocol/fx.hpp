#pragma once

// Effect types and their parameters (docs/spec/protocol.md 5.7). The tree holds every effect
// parameter as /track/N/fx/M/param/<key> (float); this catalog says which keys a built-in effect has
// and their ranges, so the engine, the mock server and the UIs agree. VST3 plugin parameters are
// normalized 0..1 and keyed "p<ParamID>".

#include "dc/protocol/errors.hpp"
#include "dc/protocol/param_tree.hpp"
#include "dc/protocol/schema.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dc::proto {

struct FxParamSpec {
    std::string key;  // path segment, e.g. "threshold_db"
    std::string name; // Japanese label for UIs
    std::string unit; // "dB", "Hz", "ms", "" ...
    double min = 0;
    double max = 1;
    double defaultValue = 0;
    int steps = 0;     // 0 = continuous, 2 = switch (0 / 1)
    bool log = false;  // UIs should map sliders logarithmically (frequencies, times)
};

struct FxTypeSpec {
    std::string type; // "eq4", "comp", ...
    std::string name; // Japanese display name, the default /fx/#/name
    std::vector<FxParamSpec> params;
    int latencySamples = 0; // added delay at 48 kHz
};

inline constexpr std::string_view kVst3TypePrefix = "vst3:";
inline constexpr int kMaxFxPublishedParams = 1024; // VST3 parameters in the tree per instance

// The built-in effects in display order.
const std::vector<FxTypeSpec>& builtinFx();
const FxTypeSpec* findBuiltinFx(std::string_view type);
const FxParamSpec* findFxParam(const FxTypeSpec& type, std::string_view key);

inline bool isVst3Type(std::string_view type) { return type.substr(0, kVst3TypePrefix.size()) == kVst3TypePrefix; }

// "p<ParamID>" <-> VST3 ParamID.
std::string vst3ParamKey(std::uint32_t paramId);
std::optional<std::uint32_t> vst3ParamId(std::string_view key);

// Splits "/track/3/fx/2/param/ratio" into the slot base "/track/3/fx/2" and the key "ratio".
struct FxParamPath {
    std::string slot;
    std::string key;
};
std::optional<FxParamPath> parseFxParamPath(std::string_view path);

// Range check of effect parameters against the slot's type in the tree (built-in ranges, VST3 0..1),
// and "editor" only for VST3 slots. Part of defaultSemanticValidator().
ValidationError validateFxChanges(const ParamTree& after, const std::vector<std::string>& changedPaths);

} // namespace dc::proto
