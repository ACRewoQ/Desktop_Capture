#pragma once

#include "dc/protocol/messages.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dc::proto {

// Flat map from path to value plus the revision it reflects. Clients keep one as a mirror of the
// server; the server keeps the authoritative one inside ParamStore.
class ParamTree {
public:
    using Map = std::map<std::string, Value, std::less<>>;

    std::uint64_t rev() const { return rev_; }
    const Map& values() const { return values_; }
    std::size_t size() const { return values_.size(); }

    const Value* get(std::string_view path) const;
    bool contains(std::string_view path) const { return get(path) != nullptr; }

    template <typename T>
    std::optional<T> getAs(std::string_view path) const {
        if (const Value* v = get(path)) {
            if (const T* x = std::get_if<T>(v)) return *x;
        }
        return std::nullopt;
    }

    void set(const std::string& path, Value v) { values_[path] = std::move(v); }

    // Removes the path itself and everything below it ("/track/3" removes "/track/3/...").
    std::size_t removeSubtree(std::string_view prefix);

    // Lists paths directly or indirectly below prefix.
    std::vector<std::string> pathsUnder(std::string_view prefix) const;

    void setRev(std::uint64_t rev) { rev_ = rev; }

    // Replaces the whole content.
    void load(const Snapshot& snap);
    Snapshot toSnapshot() const;

    enum class ApplyResult { Applied, Stale, Gap };
    // Applies a delta if delta.rev == rev() + 1. Returns Stale for old deltas (ignored) and Gap when
    // deltas were missed; the client must then call /session/resync.
    ApplyResult apply(const Delta& delta);

    bool operator==(const ParamTree& other) const { return rev_ == other.rev_ && values_ == other.values_; }

private:
    std::uint64_t rev_ = 0;
    Map values_;
};

} // namespace dc::proto
