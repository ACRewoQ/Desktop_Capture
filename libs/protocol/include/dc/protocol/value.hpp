#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace dc::proto {

// A parameter value. The tree only holds scalars; lists are expressed as indexed sub-paths
// (e.g. /track/3/out/1/target, /track/3/out/2/target).
using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

enum class ValueType : std::uint8_t { Nil, Bool, Int, Float, String };

inline ValueType typeOf(const Value& v) { return static_cast<ValueType>(v.index()); }

const char* typeName(ValueType t);

// Human readable form for logs and the CLI (strings are quoted).
std::string toDisplayString(const Value& v);

inline std::optional<double> asNumber(const Value& v) {
    if (auto* i = std::get_if<std::int64_t>(&v)) return static_cast<double>(*i);
    if (auto* d = std::get_if<double>(&v)) return *d;
    return std::nullopt;
}

} // namespace dc::proto
