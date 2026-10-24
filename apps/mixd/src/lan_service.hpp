#pragma once

// The engine's LAN side (docs/spec/protocol.md 8 and 10, docs/spec/lan-media.md), on the network thread:
//
//   send    /lan/stream/<n> becomes a port "lan:<n>" with an AudioSender while somebody subscribed to
//           audio/<n>; the engine pushes the bus into it, a pump thread cuts packets and sends them to
//           every subscriber.
//   receive a track input "lan" + "<peer>/<n>" becomes a port "lan:<peer>/<n>" with an AudioReceiver
//           (drift-corrected to the engine clock), fed by the UDP receive thread; the PeerLink of that
//           peer holds the subscription.
//   control subscriptions (/lan/subscribe, /lan/sub/#), pairing (/remote/pairing/*, the server's
//           authorize hook, /remote/paired/#), peers found by mDNS or added by address (/lan/peers/#).
//
// The controller merges ports() into the device ports before each plan and calls onDelta / onClient /
// tick; the service asks for a new plan through the callback whenever its ports change.

#include "discovery.hpp"
#include "lan_peer_link.hpp"
#include "lan_store.hpp"
#include "plan_builder.hpp"

#include "dc/control/server.hpp"
#include "dc/lanmedia/udp_transport.hpp"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace dc::mixd {

class LanService {
public:
    struct Options {
        std::string storePath;                 // "" = in memory (tests)
        std::uint16_t mediaPort = lan::kDefaultMediaPort; // 0 = any free port (tests)
        bool discovery = true;                 // advertise and browse with mDNS (Windows)
        bool verbose = true;
    };

    // Loads the store (and creates the server id) right away, so the id can go into the server options.
    LanService(asio::io_context& io, Options options);
    ~LanService();
    LanService(const LanService&) = delete;
    LanService& operator=(const LanService&) = delete;

    // Network thread, after the tree was seeded: registers the methods, opens the UDP socket, starts
    // the pump thread, the discovery and the links to paired peers. controlPort is advertised by mDNS.
    void start(control::ControlServer& server, std::uint16_t controlPort, std::function<void()> portsChanged);
    void stop();

    // ControlServer::Options::authorize.
    control::AuthDecision authorize(const proto::Hello& hello, const control::ClientInfo& client);

    // Controller hooks (network thread).
    void onDelta(const proto::Delta& delta);
    void onClient(const control::ClientInfo& client, bool connected);
    void tick(); // ~2 Hz: stream / input states, delay, pairing timeout
    void addPorts(PortTable& ports) const;

    std::uint16_t mediaPort() const;
    const std::string& serverId() const { return store_.serverId(); }

private:
    struct Subscription {
        int id = 0;
        std::string stream; // audio/<n> | video/preview | video/lan
        lan::UdpTransport::Endpoint endpoint;
        std::uint64_t connection = 0;
        std::string client;
    };
    struct StreamConfig {
        std::string codec = "pcm24";
        int opusFrames = 480;
        int bitrateKbps = 128;
        bool operator==(const StreamConfig&) const = default;
    };
    struct SendStream {
        StreamConfig config;
        std::shared_ptr<OutputPort> port; // with port->lan, while it has subscribers
        std::vector<lan::UdpTransport::Endpoint> to;
    };
    struct Peer {
        std::string id;      // server id, or "addr:<ip:port>" for a manual peer not reached yet
        std::string name;
        std::string host;
        std::uint16_t port = 0;
        bool manual = false;
        bool discovered = false;
        PeerLinkStatus link;
        std::unique_ptr<PeerLink> conn;
    };
    struct LanInput {
        std::string peer;
        int stream = 0;
        double targetMs = 0;
        std::uint32_t streamId = 0;
        std::string host; // the peer's address: only its packets are accepted
        std::shared_ptr<InputPort> port;
    };

    proto::Result subscribe(control::CallContext& ctx);
    proto::Result unsubscribe(control::CallContext& ctx);
    proto::Result pairingStart(control::CallContext& ctx);
    proto::Result pairingStop(control::CallContext& ctx);
    proto::Result unpair(control::CallContext& ctx);
    proto::Result peerPair(control::CallContext& ctx);
    proto::Result peerForget(control::CallContext& ctx);
    proto::Result peerAdd(control::CallContext& ctx);

    void removeSubscription(int id);
    void reconcileStreams();
    void reconcileInputs();
    void reconcilePeers();
    void publishPeers();
    void publishPaired();
    void onDiscovered(std::vector<DiscoveredPeer> list);
    void onLinkStatus(const std::string& key, const PeerLinkStatus& status);
    void onDatagram(std::span<const std::uint8_t> d, const lan::UdpTransport::Endpoint& from, double arrival);
    void pumpLoop();
    void changed();
    Peer* findPeer(const std::string& id);
    std::string instanceName() const;

    asio::io_context& io_;
    control::ControlServer* server_ = nullptr;
    Options opts_;
    LanStore store_;
    PairingWindow pairing_;
    std::function<void()> portsChanged_;
    std::uint16_t controlPort_ = 0;
    bool started_ = false;
    std::atomic<bool> stopping_{false};

    std::vector<Subscription> subs_;
    std::array<SendStream, proto::kMaxLanStreams> streams_{};
    std::map<std::string, Peer> peers_;          // by id (or "addr:...")
    std::map<std::string, LanInput> inputs_;     // by port key "lan:<peer>/<n>"
    std::vector<DiscoveredPeer> discovered_;
    std::unique_ptr<Discovery> discovery_;
    std::string advertisedName_;

    std::unique_ptr<lan::UdpTransport> transport_;
    // shared with the UDP receive thread and the pump thread
    mutable std::mutex mediaMutex_;
    struct MediaSend {
        std::shared_ptr<lan::AudioSender> sender;
        std::vector<lan::UdpTransport::Endpoint> to;
    };
    struct MediaRecv {
        std::uint32_t streamId = 0;
        std::shared_ptr<lan::AudioReceiver> receiver;
        asio::ip::address peer; // only packets from this address
        bool haveSource = false;
        lan::UdpTransport::Endpoint source; // where the stream comes from: reports go back there
    };
    std::vector<MediaSend> mediaSend_;
    std::vector<MediaRecv> mediaRecv_;
    void publishMedia(); // copies streams_ / inputs_ into the media tables

    double nextStatsLog_ = 0; // tick(): receive statistics to the log every 30 s

    std::thread pump_;
    std::atomic<bool> pumpRunning_{false};
};

} // namespace dc::mixd
