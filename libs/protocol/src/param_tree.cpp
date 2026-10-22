#include "dc/protocol/param_tree.hpp"

namespace dc::proto {

namespace {

bool isUnder(std::string_view path, std::string_view prefix) {
    if (path.size() < prefix.size() || path.substr(0, prefix.size()) != prefix) return false;
    return path.size() == prefix.size() || path[prefix.size()] == '/';
}

} // namespace

const Value* ParamTree::get(std::string_view path) const {
    auto it = values_.find(path);
    return it == values_.end() ? nullptr : &it->second;
}

std::size_t ParamTree::removeSubtree(std::string_view prefix) {
    std::size_t n = 0;
    for (auto it = values_.lower_bound(prefix); it != values_.end();) {
        if (isUnder(it->first, prefix)) {
            it = values_.erase(it);
            ++n;
        } else if (it->first.compare(0, prefix.size(), prefix) == 0) {
            ++it; // e.g. "/track/10" while removing "/track/1"
        } else {
            break;
        }
    }
    return n;
}

std::vector<std::string> ParamTree::pathsUnder(std::string_view prefix) const {
    std::vector<std::string> out;
    for (auto it = values_.lower_bound(prefix); it != values_.end(); ++it) {
        if (it->first.compare(0, prefix.size(), prefix) != 0) break;
        if (isUnder(it->first, prefix)) out.push_back(it->first);
    }
    return out;
}

void ParamTree::load(const Snapshot& snap) {
    values_.clear();
    for (const auto& c : snap.params) values_[c.path] = c.value;
    rev_ = snap.rev;
}

Snapshot ParamTree::toSnapshot() const {
    Snapshot s;
    s.rev = rev_;
    s.params.reserve(values_.size());
    for (const auto& [k, v] : values_) s.params.push_back({k, v});
    return s;
}

ParamTree::ApplyResult ParamTree::apply(const Delta& delta) {
    if (delta.rev <= rev_) return ApplyResult::Stale;
    if (delta.rev != rev_ + 1) return ApplyResult::Gap;
    // Removals first so a delta can remove a subtree and recreate parts of it.
    for (const auto& r : delta.removed) removeSubtree(r);
    for (const auto& c : delta.changes) values_[c.path] = c.value;
    rev_ = delta.rev;
    return ApplyResult::Applied;
}

} // namespace dc::proto
