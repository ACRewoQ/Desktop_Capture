#pragma once

#include "dc/protocol/codec.hpp"
#include "dc/protocol/constants.hpp"
#include "dc/protocol/param_tree.hpp"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace dc::control {

// Control protocol client with its own network thread. Keeps a mirror of the server's parameter tree.
// Used by capd (to follow recording settings), the CLI tool and tests. The Qt UI will wrap the same
// codec and ParamTree around Qt's own sockets in P2.
class ControlClient {
public:
    struct Options {
        std::string host = "127.0.0.1";
        std::uint16_t port = proto::kDefaultControlPort;
        std::string clientName = "client";
        proto::ClientRole role = proto::ClientRole::Local;
        std::uint16_t meterPort = 0;
        std::string token;
        std::string pairCode;
        std::string clientId;
        std::chrono::milliseconds pingInterval{5000};
    };

    explicit ControlClient(Options options);
    ~ControlClient();

    ControlClient(const ControlClient&) = delete;
    ControlClient& operator=(const ControlClient&) = delete;

    // Connects, sends Hello and waits until Welcome and the first Snapshot arrived.
    bool connect(std::chrono::milliseconds timeout, std::string* error = nullptr);
    void disconnect();
    bool connected() const { return connected_.load(); }

    std::future<proto::Result> set(std::vector<proto::Change> changes);
    std::future<proto::Result> call(std::string method, proto::Args args = {});

    proto::ParamTree tree() const;
    proto::Welcome welcome() const;

    // Blocks until the mirror reaches at least rev (or the timeout expires). The onDelta / onSnapshot
    // callbacks for that revision have run when it returns true.
    bool waitForRev(std::uint64_t rev, std::chrono::milliseconds timeout) const;

    // Callbacks run on the network thread. Set them before connect().
    std::function<void(const proto::Snapshot&)> onSnapshot;
    std::function<void(const proto::Delta&)> onDelta;
    std::function<void(const proto::Event&)> onEvent;
    std::function<void(const std::string& reason)> onDisconnected;

private:
    void read();
    void send(proto::Message msg);
    void writeNext();
    void handle(proto::Message msg);
    void failAll(const std::string& reason);
    void schedulePing();

    Options options_;
    asio::io_context io_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer pingTimer_;
    std::thread thread_;
    std::array<std::uint8_t, 64 * 1024> readBuf_{};
    proto::FrameDecoder decoder_;
    std::deque<proto::Bytes> queue_;

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    proto::ParamTree tree_;
    proto::Welcome welcome_;
    bool gotWelcome_ = false;
    bool gotSnapshot_ = false;
    std::string lastError_;
    std::map<std::uint64_t, std::promise<proto::Result>> pending_;
    std::uint64_t nextId_ = 1;
    std::atomic<bool> connected_{false};
    bool resyncRequested_ = false;
};

} // namespace dc::control
