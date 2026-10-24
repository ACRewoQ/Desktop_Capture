#pragma once

// Reading the engine's LAN subscriptions (/lan/sub/#, docs/spec/protocol.md 10) from a tree mirror:
// capd sends the video streams to the subscribers the engine lists.

#include "dc/protocol/param_tree.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace dc::proto {

struct LanSubscription {
    int id = 0;
    std::string stream; // audio/<n> | video/preview | video/lan
    std::string host;   // IPv4 address of the subscriber
    std::uint16_t port = 0;
    std::string client;
    bool operator==(const LanSubscription&) const = default;
};

// Every well-formed /lan/sub/# entry, by id; with stream non-empty, only that stream's.
std::vector<LanSubscription> lanSubscriptions(const ParamTree& tree, const std::string& stream = {});

} // namespace dc::proto
