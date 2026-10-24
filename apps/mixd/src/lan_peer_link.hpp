#pragma once

// The engine's connection to one paired peer engine (role Peer, docs/spec/protocol.md 10.1): keeps a
// control connection open, mirrors the peer's LAN send streams (name, active, stream id) and holds the
// subscriptions the tracks here need. Runs on its own thread so that connecting never blocks the
// engine's network thread.

#include "dc/protocol/schema.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace dc::mixd {

struct PeerStreamInfo {
    std::string name;
    bool active = false;
    std::uint32_t streamId = 0;
    bool operator==(const PeerStreamInfo&) const = default;
};

struct PeerLinkStatus {
    bool connected = false;
    bool refused = false;   // the peer rejected our token (unpaired on its side)
    std::string serverId;   // Welcome.server_id
    std::string serverName; // the peer's /lan/name, or its computer name
    std::array<PeerStreamInfo, proto::kMaxLanStreams> streams{};
    std::set<int> subscribed; // audio/<n> held right now
    bool operator==(const PeerLinkStatus&) const = default;
};

class PeerLink {
public:
    struct Options {
        std::string host;
        std::uint16_t port = 0;
        std::string selfId;   // our server id (Hello.client_id)
        std::string selfName; // Hello.client
        std::string token;
        std::uint16_t mediaPort = 0; // our UDP port: where the peer sends the streams
    };
    // Called on the link thread whenever the status changed.
    using Update = std::function<void(const PeerLinkStatus&)>;

    PeerLink(Options options, Update update);
    ~PeerLink();
    PeerLink(const PeerLink&) = delete;
    PeerLink& operator=(const PeerLink&) = delete;

    // The audio streams (1..8) the tracks here use. Any thread.
    void setWanted(std::set<int> streams);
    const Options& options() const { return opts_; }

private:
    void run();

    Options opts_;
    Update update_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::set<int> wanted_;
    bool stop_ = false;
    std::atomic<bool> dirty_{true};
    std::thread thread_;
};

} // namespace dc::mixd
