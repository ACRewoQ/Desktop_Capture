#pragma once

#include "dc/protocol/codec.hpp"
#include "dc/protocol/meter.hpp"
#include "dc/protocol/param_store.hpp"

#include <asio.hpp>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dc::control {

struct ClientInfo {
    std::uint64_t connectionId = 0;
    std::string name;
    proto::ClientRole role = proto::ClientRole::Local;
    std::string address;
    bool loopback = true;
};

class ControlServer;

struct CallContext {
    const proto::Call& call;
    const ClientInfo& client;
    ControlServer& server;
};

// TCP control server shared by the engine (mixd) and the mock server. Runs entirely on the thread
// that runs the given io_context; only publish(), sendMeters(), broadcastEvent() and stop() may be
// called from other threads.
class ControlServer {
public:
    struct Options {
        std::string bindAddress = "127.0.0.1";
        std::uint16_t port = 0; // 0 = pick a free port (tests)
        std::string serverName = "mixd";
        std::string serverId;
        std::uint64_t sessionId = 0;
        bool allowRemote = false;            // accept non-loopback clients (pairing comes in P4)
        std::size_t maxRemoteClients = 4;
        std::chrono::milliseconds idleTimeout{15000};
        // Optional log hook: every decoded client message (after Hello) and connection changes.
        std::function<void(const ClientInfo&, const std::string& text)> log;
    };

    // Returns the Result to send. Handlers may call publishNow() to change the tree; those deltas
    // are broadcast before the Result.
    using CallHandler = std::function<proto::Result(CallContext&)>;

    ControlServer(asio::io_context& io, proto::ParamStore& store, Options options);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Method patterns use "#" for index segments, e.g. "/track/#/fx/add".
    void registerMethod(const std::string& pattern, CallHandler handler);

    void start();
    void stop();

    std::uint16_t port() const { return port_; }
    std::size_t clientCount() const;

    // Network-thread only: apply an engine-side change and broadcast the delta immediately.
    void publishNow(const std::vector<proto::Change>& changes, const std::vector<std::string>& removed = {},
                    std::uint64_t originId = 0, const std::string& originClient = {});
    void broadcastEventNow(const proto::Event& ev);

    // Thread-safe wrappers that post to the network thread.
    void publish(std::vector<proto::Change> changes, std::vector<std::string> removed = {});
    void broadcastEvent(proto::Event ev);
    void sendMeters(proto::MeterPacket packet);

    proto::ParamStore& store() { return store_; }

private:
    class Session;
    friend class Session;

    void accept();
    void onMessage(const std::shared_ptr<Session>& s, proto::Message msg);
    void handleHello(const std::shared_ptr<Session>& s, const proto::Hello& hello);
    void handleCall(const std::shared_ptr<Session>& s, const proto::Call& call);
    void broadcast(const proto::Message& msg);
    void removeSession(std::uint64_t id);
    const CallHandler* findHandler(const std::string& method) const;

    asio::io_context& io_;
    proto::ParamStore& store_;
    Options options_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::udp::socket meterSocket_;
    std::uint16_t port_ = 0;
    std::uint64_t nextSessionId_ = 1;
    std::atomic<std::size_t> readyClients_{0};
    std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;
    std::unordered_map<std::string, CallHandler> handlers_;
};

} // namespace dc::control
