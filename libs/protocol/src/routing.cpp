#include "dc/protocol/routing.hpp"

#include <algorithm>
#include <charconv>
#include <functional>
#include <map>
#include <set>

namespace dc::proto {

namespace {

// Returns N for paths shaped like /track/N/<rest...>, else 0.
int trackOf(std::string_view path, std::string_view& rest) {
    constexpr std::string_view kPrefix = "/track/";
    if (path.substr(0, kPrefix.size()) != kPrefix) return 0;
    const auto tail = path.substr(kPrefix.size());
    const auto slash = tail.find('/');
    const auto num = tail.substr(0, slash);
    int n = 0;
    auto [p, ec] = std::from_chars(num.data(), num.data() + num.size(), n);
    if (ec != std::errc() || p != num.data() + num.size()) return 0;
    rest = slash == std::string_view::npos ? std::string_view{} : tail.substr(slash);
    return n;
}

bool isTargetPath(std::string_view rest) {
    // "/out/K/target" or "/send/K/target"
    if (rest.size() < 8 || rest.substr(rest.size() - 7) != "/target") return false;
    return rest.substr(0, 5) == "/out/" || rest.substr(0, 6) == "/send/";
}

std::set<int> existingTracks(const ParamTree& tree) {
    std::set<int> out;
    for (const auto& [path, v] : tree.values()) {
        std::string_view rest;
        if (int n = trackOf(path, rest); n > 0 && rest == "/position") out.insert(n);
    }
    return out;
}

// track -> tracks it feeds
std::map<int, std::set<int>> buildGraph(const ParamTree& tree) {
    std::map<int, std::set<int>> g;
    for (const auto& [path, v] : tree.values()) {
        std::string_view rest;
        const int n = trackOf(path, rest);
        if (n == 0 || !isTargetPath(rest)) continue;
        const auto* s = std::get_if<std::string>(&v);
        if (!s) continue;
        if (auto t = parseTarget(*s); t && t->kind == RouteTarget::Kind::Track) g[n].insert(t->track);
    }
    return g;
}

} // namespace

std::optional<RouteTarget> parseTarget(std::string_view s) {
    RouteTarget t;
    if (s.empty()) return t;
    if (s == "master") {
        t.kind = RouteTarget::Kind::Master;
        return t;
    }
    const auto slash = s.find('/');
    if (slash == std::string_view::npos || slash + 1 >= s.size()) return std::nullopt;
    const auto kind = s.substr(0, slash);
    const auto arg = s.substr(slash + 1);
    if (kind == "track") {
        int n = 0;
        auto [p, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), n);
        if (ec != std::errc() || p != arg.data() + arg.size() || n < 1 || n > kMaxTracks) return std::nullopt;
        t.kind = RouteTarget::Kind::Track;
        t.track = n;
        return t;
    }
    if (kind == "device") t.kind = RouteTarget::Kind::Device;
    else if (kind == "vmic") t.kind = RouteTarget::Kind::VirtualMic;
    else if (kind == "lan") t.kind = RouteTarget::Kind::Lan;
    else return std::nullopt;
    t.id = std::string(arg);
    return t;
}

int lanStreamOf(const RouteTarget& t) {
    if (t.kind != RouteTarget::Kind::Lan) return 0;
    int n = 0;
    auto [p, ec] = std::from_chars(t.id.data(), t.id.data() + t.id.size(), n);
    if (ec != std::errc() || p != t.id.data() + t.id.size() || n < 1 || n > kMaxLanStreams) return 0;
    if (t.id[0] == '0') return 0;
    return n;
}

std::optional<std::vector<int>> findRoutingCycle(const ParamTree& tree) {
    const auto g = buildGraph(tree);
    std::map<int, int> state; // 0 = unvisited, 1 = on stack, 2 = done
    std::vector<int> stack;
    std::optional<std::vector<int>> found;

    std::function<bool(int)> dfs = [&](int u) {
        state[u] = 1;
        stack.push_back(u);
        if (auto it = g.find(u); it != g.end()) {
            for (int v : it->second) {
                if (state[v] == 1) {
                    auto start = std::find(stack.begin(), stack.end(), v);
                    std::vector<int> cycle(start, stack.end());
                    cycle.push_back(v);
                    found = std::move(cycle);
                    return true;
                }
                if (state[v] == 0 && dfs(v)) return true;
            }
        }
        stack.pop_back();
        state[u] = 2;
        return false;
    };

    for (const auto& [u, _] : g) {
        if (state[u] == 0 && dfs(u)) break;
    }
    return found;
}

std::vector<int> processingOrder(const ParamTree& tree) {
    const auto tracks = existingTracks(tree);
    const auto g = buildGraph(tree);
    std::map<int, int> indegree;
    for (int t : tracks) indegree[t] = 0;
    for (const auto& [u, vs] : g)
        for (int v : vs)
            if (tracks.count(u) && tracks.count(v)) ++indegree[v];

    std::set<int> ready;
    for (const auto& [t, d] : indegree)
        if (d == 0) ready.insert(t);

    std::vector<int> order;
    while (!ready.empty()) {
        const int u = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(u);
        if (auto it = g.find(u); it != g.end()) {
            for (int v : it->second) {
                if (!tracks.count(v)) continue;
                if (--indegree[v] == 0) ready.insert(v);
            }
        }
    }
    if (order.size() != tracks.size()) return {};
    return order;
}

ValidationError validateRouting(const ParamTree& tree) {
    const auto tracks = existingTracks(tree);
    for (const auto& [path, v] : tree.values()) {
        std::string_view rest;
        const int n = trackOf(path, rest);
        const bool isMasterOut = path.rfind("/master/out/", 0) == 0 && path.size() > 7 &&
                                 path.substr(path.size() - 7) == "/target";
        if (!isMasterOut && (n == 0 || !isTargetPath(rest))) continue;

        const auto* s = std::get_if<std::string>(&v);
        if (!s) continue;
        const auto t = parseTarget(*s);
        if (!t) return {ErrorCode::OutOfRange, path + ": invalid target \"" + *s + "\""};
        if (t->kind == RouteTarget::Kind::Lan && lanStreamOf(*t) == 0)
            return {ErrorCode::OutOfRange, path + ": LAN target must be lan/1 .. lan/" + std::to_string(kMaxLanStreams)};

        if (isMasterOut) {
            if (t->kind == RouteTarget::Kind::Master || t->kind == RouteTarget::Kind::Track)
                return {ErrorCode::Conflict, path + ": master can only output to devices, virtual mics or LAN"};
            continue;
        }
        const bool isSend = rest.substr(0, 6) == "/send/";
        if (isSend && t->kind == RouteTarget::Kind::Master)
            return {ErrorCode::Conflict, path + ": sends cannot target master (use an output instead)"};
        if (t->kind == RouteTarget::Kind::Track) {
            if (t->track == n) return {ErrorCode::Conflict, path + ": a track cannot output to itself"};
            if (!tracks.count(t->track))
                return {ErrorCode::NotFound, path + ": track " + std::to_string(t->track) + " does not exist"};
        }
    }
    if (auto cycle = findRoutingCycle(tree)) {
        std::string text;
        for (int t : *cycle) text += (text.empty() ? "" : " -> ") + std::string("track/") + std::to_string(t);
        return {ErrorCode::Conflict, "routing loop: " + text};
    }
    return {};
}

} // namespace dc::proto
