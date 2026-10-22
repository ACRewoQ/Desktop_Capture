#include "dc/protocol/schema.hpp"

#include <algorithm>
#include <cmath>

namespace dc::proto {

namespace {

bool isIndexSegment(std::string_view seg) {
    if (seg.empty() || seg.size() > 9 || seg[0] == '0') return false;
    return std::all_of(seg.begin(), seg.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string withLastSegmentWildcard(std::string_view pattern) {
    const auto pos = pattern.rfind('/');
    return std::string(pattern.substr(0, pos + 1)) + "*";
}

} // namespace

bool isValidPath(std::string_view path) {
    if (path.size() < 2 || path.front() != '/' || path.back() == '/') return false;
    std::size_t start = 1;
    while (start <= path.size()) {
        auto end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        const auto seg = path.substr(start, end - start);
        if (seg.empty()) return false;
        const bool digits = std::all_of(seg.begin(), seg.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (digits && !isIndexSegment(seg)) return false;
        for (char c : seg) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            if (!ok) return false;
        }
        start = end + 1;
    }
    return true;
}

std::string Schema::patternOf(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    std::size_t start = 0;
    while (start < path.size()) {
        auto end = path.find('/', start + 1);
        if (end == std::string_view::npos) end = path.size();
        const auto seg = path.substr(start + 1, end - start - 1);
        out += '/';
        if (isIndexSegment(seg)) out += '#';
        else out += seg;
        start = end;
    }
    return out;
}

void Schema::add(ParamSpec spec) {
    index_[spec.pattern] = params_.size();
    params_.push_back(std::move(spec));
}

void Schema::addMethod(MethodSpec spec) {
    methodIndex_[spec.pattern] = methods_.size();
    methods_.push_back(std::move(spec));
}

const ParamSpec* Schema::find(std::string_view path) const {
    const auto pattern = patternOf(path);
    if (auto it = index_.find(pattern); it != index_.end()) return &params_[it->second];
    if (auto it = index_.find(withLastSegmentWildcard(pattern)); it != index_.end()) return &params_[it->second];
    return nullptr;
}

const MethodSpec* Schema::findMethod(std::string_view method) const {
    if (auto it = methodIndex_.find(patternOf(method)); it != methodIndex_.end()) return &methods_[it->second];
    return nullptr;
}

ValidationError Schema::validate(const ParamSpec& spec, const Value& v, bool fromClient) const {
    if (fromClient && spec.access == Access::ReadOnly)
        return {ErrorCode::ReadOnly, spec.pattern + " is read-only"};

    const auto actual = typeOf(v);
    const bool typeOk = actual == spec.type || (spec.type == ValueType::Float && actual == ValueType::Int);
    if (!typeOk)
        return {ErrorCode::TypeMismatch,
                std::string("expected ") + typeName(spec.type) + ", got " + typeName(actual)};

    if (auto n = asNumber(v)) {
        if (!std::isfinite(*n)) return {ErrorCode::OutOfRange, "value must be finite"};
        if ((spec.min && *n < *spec.min) || (spec.max && *n > *spec.max))
            return {ErrorCode::OutOfRange, toDisplayString(v) + " is outside the allowed range"};
        if (!spec.intChoices.empty()) {
            const auto i = std::get<std::int64_t>(v);
            if (std::find(spec.intChoices.begin(), spec.intChoices.end(), i) == spec.intChoices.end())
                return {ErrorCode::OutOfRange, toDisplayString(v) + " is not an allowed value"};
        }
    }
    if (!spec.choices.empty()) {
        const auto& s = std::get<std::string>(v);
        if (std::find(spec.choices.begin(), spec.choices.end(), s) == spec.choices.end())
            return {ErrorCode::OutOfRange, "\"" + s + "\" is not an allowed choice"};
    }
    return {};
}

Value Schema::normalize(const ParamSpec& spec, Value v) {
    if (spec.type == ValueType::Float) {
        if (auto* i = std::get_if<std::int64_t>(&v)) return static_cast<double>(*i);
    }
    return v;
}

} // namespace dc::proto
