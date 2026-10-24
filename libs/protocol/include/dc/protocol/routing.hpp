#pragma once

#include "dc/protocol/param_tree.hpp"
#include "dc/protocol/schema.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dc::proto {

// Parsed form of an output / send target string. See docs/spec/protocol.md "出力先の書き方".
struct RouteTarget {
    enum class Kind { None, Master, Track, Device, VirtualMic, Lan };
    Kind kind = Kind::None;
    int track = 0;  // Kind::Track
    std::string id; // Device / VirtualMic / Lan
};

std::optional<RouteTarget> parseTarget(std::string_view s);

// Stream number of a LAN target ("lan/3" -> 3), 0 if it is not lan/1 .. lan/kMaxLanStreams.
int lanStreamOf(const RouteTarget& t);

// Checks every /track/#/out, /track/#/send and /master/out target in the tree:
// syntax, master restrictions, referenced tracks exist, no self routing and no cycles.
ValidationError validateRouting(const ParamTree& tree);

// Returns the track numbers of one cycle (first == last) if the routing graph has one.
std::optional<std::vector<int>> findRoutingCycle(const ParamTree& tree);

// Topological processing order of existing tracks (sources first). Empty if there is a cycle.
std::vector<int> processingOrder(const ParamTree& tree);

} // namespace dc::proto
