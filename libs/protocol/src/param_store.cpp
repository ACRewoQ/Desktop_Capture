#include "dc/protocol/param_store.hpp"
#include "dc/protocol/routing.hpp"
#include "dc/protocol/video.hpp"

#include <stdexcept>

namespace dc::proto {

ValidationError defaultSemanticValidator(const ParamTree& after, const std::vector<Change>& changes) {
    if (auto err = validateVideo(after, changes)) return err;
    for (const auto& c : changes) {
        if (c.path.size() > 7 && c.path.compare(c.path.size() - 7, 7, "/target") == 0) return validateRouting(after);
    }
    return {};
}

ParamStore::ParamStore(const Schema& schema) : schema_(schema), validator_(defaultSemanticValidator) {}

ParamStore::Outcome ParamStore::applySet(const Set& set, const std::string& clientName) {
    Outcome out;
    out.result.id = set.id;

    auto fail = [&](ErrorCode code, std::string message, const std::string& path) {
        out.result.code = code;
        out.result.message = std::move(message);
        out.result.path = path;
        return out;
    };

    if (set.changes.empty()) return fail(ErrorCode::BadRequest, "no changes", {});

    std::vector<Change> normalized;
    normalized.reserve(set.changes.size());
    for (const auto& c : set.changes) {
        if (!isValidPath(c.path)) return fail(ErrorCode::BadRequest, "invalid path syntax", c.path);
        if (!tree_.contains(c.path)) return fail(ErrorCode::NotFound, "no such parameter", c.path);
        const ParamSpec* spec = schema_.find(c.path);
        if (!spec) return fail(ErrorCode::NotFound, "parameter not in schema", c.path);
        if (auto err = schema_.validate(*spec, c.value, true)) return fail(err.code, err.message, c.path);
        normalized.push_back({c.path, Schema::normalize(*spec, c.value)});
    }

    // Apply tentatively, remembering old values for rollback.
    std::vector<Change> previous;
    std::vector<Change> effective;
    for (const auto& c : normalized) {
        const Value& old = *tree_.get(c.path);
        if (old == c.value) continue;
        previous.push_back({c.path, old});
        effective.push_back(c);
        tree_.set(c.path, c.value);
    }

    if (validator_ && !effective.empty()) {
        if (auto err = validator_(tree_, effective)) {
            for (auto it = previous.rbegin(); it != previous.rend(); ++it) tree_.set(it->path, it->value);
            return fail(err.code, err.message, {});
        }
    }

    if (!effective.empty()) {
        Delta d;
        d.rev = tree_.rev() + 1;
        d.originId = set.id;
        d.originClient = clientName;
        d.changes = std::move(effective);
        tree_.setRev(d.rev);
        out.delta = std::move(d);
    }
    return out;
}

std::optional<Delta> ParamStore::publish(const std::vector<Change>& changes, const std::vector<std::string>& removed,
                                         std::uint64_t originId, const std::string& originClient) {
    Delta d;
    d.originId = originId;
    d.originClient = originClient;

    for (const auto& r : removed) {
        if (tree_.removeSubtree(r) > 0) d.removed.push_back(r);
    }
    for (const auto& c : changes) {
        if (!isValidPath(c.path)) throw std::invalid_argument("invalid path: " + c.path);
        const ParamSpec* spec = schema_.find(c.path);
        if (!spec) throw std::invalid_argument("path not in schema: " + c.path);
        if (auto err = schema_.validate(*spec, c.value, false))
            throw std::invalid_argument(c.path + ": " + err.message);
        Value v = Schema::normalize(*spec, c.value);
        if (const Value* old = tree_.get(c.path); old && *old == v) continue;
        tree_.set(c.path, v);
        d.changes.push_back({c.path, std::move(v)});
    }

    if (d.changes.empty() && d.removed.empty()) return std::nullopt;
    d.rev = tree_.rev() + 1;
    tree_.setRev(d.rev);
    return d;
}

} // namespace dc::proto
