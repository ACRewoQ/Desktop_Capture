#include "dc/control/server.hpp"

#include "dc/protocol/constants.hpp"

#include <array>
#include <deque>
#include <iostream>

namespace dc::control {

using asio::ip::tcp;
using namespace dc::proto;

class ControlServer::Session : public std::enable_shared_from_this<ControlServer::Session> {
public:
    Session(ControlServer& server, tcp::socket socket, std::uint64_t id)
        : server_(server), socket_(std::move(socket)), idleTimer_(socket_.get_executor()) {
        info.connectionId = id;
        asio::error_code ec;
        const auto ep = socket_.remote_endpoint(ec);
        if (!ec) {
            info.address = ep.address().to_string();
            info.loopback = ep.address().is_loopback();
            remoteAddress = ep.address();
        }
    }

    void start() {
        lastActivity_ = std::chrono::steady_clock::now();
        read();
        armIdleTimer();
    }

    void send(const Message& msg) { sendBytes(std::make_shared<Bytes>(encodeFrame(msg))); }

    void sendBytes(std::shared_ptr<Bytes> frame) {
        if (closed_) return;
        const bool idle = queue_.empty();
        queue_.push_back(std::move(frame));
        if (idle) writeNext();
    }

    // Sends the remaining queue, then closes.
    void closeAfterFlush() {
        closing_ = true;
        if (queue_.empty()) close();
    }

    void close() {
        if (closed_) return;
        closed_ = true;
        asio::error_code ec;
        idleTimer_.cancel();
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        server_.removeSession(info.connectionId);
    }

    ClientInfo info;
    asio::ip::address remoteAddress;
    std::uint16_t meterPort = 0;
    bool ready = false; // Hello accepted

private:
    void read() {
        auto self = shared_from_this();
        socket_.async_read_some(asio::buffer(readBuf_), [this, self](asio::error_code ec, std::size_t n) {
            if (ec) {
                close();
                return;
            }
            lastActivity_ = std::chrono::steady_clock::now();
            decoder_.feed({readBuf_.data(), n});
            while (!closed_) {
                auto payload = decoder_.next();
                if (!payload) break;
                auto decoded = decodePayload(*payload);
                if (!decoded.message) {
                    send(Error{ErrorCode::BadRequest, "decode error: " + decoded.error});
                    closeAfterFlush();
                    return;
                }
                server_.onMessage(self, std::move(*decoded.message));
            }
            if (decoder_.failed()) {
                send(Error{ErrorCode::BadRequest, "frame too large"});
                closeAfterFlush();
                return;
            }
            if (!closed_ && !closing_) read();
        });
    }

    void writeNext() {
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(*queue_.front()), [this, self](asio::error_code ec, std::size_t) {
            if (ec) {
                close();
                return;
            }
            queue_.pop_front();
            if (!queue_.empty()) writeNext();
            else if (closing_) close();
        });
    }

    void armIdleTimer() {
        auto self = shared_from_this();
        idleTimer_.expires_after(std::chrono::seconds(1));
        idleTimer_.async_wait([this, self](asio::error_code ec) {
            if (ec || closed_) return;
            if (std::chrono::steady_clock::now() - lastActivity_ > server_.options_.idleTimeout) {
                close();
                return;
            }
            armIdleTimer();
        });
    }

    ControlServer& server_;
    tcp::socket socket_;
    asio::steady_timer idleTimer_;
    std::array<std::uint8_t, 64 * 1024> readBuf_{};
    FrameDecoder decoder_;
    std::deque<std::shared_ptr<Bytes>> queue_;
    std::chrono::steady_clock::time_point lastActivity_;
    bool closing_ = false;
    bool closed_ = false;
};

ControlServer::ControlServer(asio::io_context& io, ParamStore& store, Options options)
    : io_(io), store_(store), options_(std::move(options)), acceptor_(io), meterSocket_(io) {
    registerMethod("/session/resync", [](CallContext&) { return Result{}; }); // snapshot is sent in handleCall
}

ControlServer::~ControlServer() = default;

void ControlServer::registerMethod(const std::string& pattern, CallHandler handler) {
    handlers_[pattern] = std::move(handler);
}

void ControlServer::start() {
    const auto address = asio::ip::make_address(options_.bindAddress);
    tcp::endpoint ep(address, options_.port);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    port_ = acceptor_.local_endpoint().port();
    meterSocket_.open(address.is_v6() ? asio::ip::udp::v6() : asio::ip::udp::v4());
    accept();
}

void ControlServer::stop() {
    asio::post(io_, [this] {
        asio::error_code ec;
        acceptor_.close(ec);
        meterSocket_.close(ec);
        auto copy = sessions_;
        for (auto& [id, s] : copy) {
            s->send(Bye{"server stopping"});
            s->closeAfterFlush();
        }
    });
}

std::size_t ControlServer::clientCount() const { return readyClients_.load(); }

void ControlServer::accept() {
    acceptor_.async_accept([this](asio::error_code ec, tcp::socket socket) {
        if (ec) {
            if (ec == asio::error::operation_aborted) return;
            accept();
            return;
        }
        socket.set_option(tcp::no_delay(true));
        const auto id = nextSessionId_++;
        auto session = std::make_shared<Session>(*this, std::move(socket), id);
        if (!session->info.loopback && !options_.allowRemote && !options_.authorize) {
            session->start();
            session->send(Error{ErrorCode::Unauthorized, "remote connections are disabled"});
            session->closeAfterFlush();
        } else {
            sessions_[id] = session;
            session->start();
        }
        accept();
    });
}

void ControlServer::disconnectIf(const std::function<bool(const ClientInfo&)>& which, const std::string& reason) {
    auto copy = sessions_;
    for (auto& [id, s] : copy)
        if (s->ready && which(s->info)) {
            s->send(Bye{reason});
            s->closeAfterFlush();
        }
}

std::vector<ClientInfo> ControlServer::clients() const {
    std::vector<ClientInfo> out;
    for (const auto& [id, s] : sessions_)
        if (s->ready) out.push_back(s->info);
    return out;
}

void ControlServer::removeSession(std::uint64_t id) {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    if (options_.log) options_.log(it->second->info, "disconnected");
    const bool wasReady = it->second->ready;
    const ClientInfo info = it->second->info;
    if (wasReady) --readyClients_;
    sessions_.erase(it);
    if (wasReady && options_.onClient) options_.onClient(info, false);
}

namespace {
std::string describe(const Message& msg) {
    std::string text = typeName(typeOf(msg));
    if (auto* hello = std::get_if<Hello>(&msg)) {
        text += " " + hello->clientName + " proto " + std::to_string(hello->proto) + " role " +
                std::to_string(static_cast<int>(hello->role));
    } else if (auto* set = std::get_if<Set>(&msg)) {
        for (const auto& c : set->changes) text += " " + c.path + "=" + toDisplayString(c.value);
    } else if (auto* call = std::get_if<Call>(&msg)) {
        text += " " + call->method;
        for (const auto& [k, v] : call->args) text += " " + k + "=" + toDisplayString(v);
    }
    return text;
}
} // namespace

void ControlServer::onMessage(const std::shared_ptr<Session>& s, Message msg) {
    if (options_.log && !std::holds_alternative<Ping>(msg)) options_.log(s->info, describe(msg));
    if (!s->ready) {
        if (auto* hello = std::get_if<Hello>(&msg)) {
            handleHello(s, *hello);
        } else {
            s->send(Error{ErrorCode::BadRequest, "first message must be Hello"});
            s->closeAfterFlush();
        }
        return;
    }

    std::visit(
        [&](auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, Set>) {
                if (s->info.role == ClientRole::Peer) {
                    Result r;
                    r.id = m.id;
                    r.code = ErrorCode::Unauthorized;
                    r.message = "a peer engine cannot change settings";
                    s->send(r);
                    return;
                }
                auto outcome = store_.applySet(m, s->info.name);
                if (outcome.delta) emitDelta(*outcome.delta);
                s->send(outcome.result);
            } else if constexpr (std::is_same_v<T, Call>) {
                handleCall(s, m);
            } else if constexpr (std::is_same_v<T, Ping>) {
                s->send(Pong{m.t});
            } else if constexpr (std::is_same_v<T, Pong>) {
                // keepalive answer, nothing to do
            } else if constexpr (std::is_same_v<T, Bye>) {
                s->close();
            } else {
                s->send(Error{ErrorCode::BadRequest, std::string("unexpected message ") + typeName(typeOf(msg))});
                s->closeAfterFlush();
            }
        },
        msg);
}

void ControlServer::handleHello(const std::shared_ptr<Session>& s, const Hello& hello) {
    if (hello.proto != kProtocolVersion) {
        s->send(Error{ErrorCode::UnsupportedVersion,
                      "server speaks protocol " + std::to_string(kProtocolVersion) + ", client " +
                          std::to_string(hello.proto)});
        s->closeAfterFlush();
        return;
    }
    std::string token;
    if (!s->info.loopback && !options_.allowRemote) {
        // only remote controllers and peer engines come from the LAN, and only after pairing
        AuthDecision d;
        if (hello.role != ClientRole::Remote && hello.role != ClientRole::Peer) d.message = "this role must connect locally";
        else d = options_.authorize(hello, s->info);
        if (!d.allowed) {
            s->send(Error{ErrorCode::Unauthorized, d.message.empty() ? "not paired" : d.message});
            s->closeAfterFlush();
            return;
        }
        token = d.token;
    }
    if (hello.role == ClientRole::Remote) {
        std::size_t remotes = 0;
        for (const auto& [id, other] : sessions_)
            if (other->ready && other->info.role == ClientRole::Remote) ++remotes;
        if (remotes >= options_.maxRemoteClients) {
            s->send(Error{ErrorCode::LimitReached, "too many remote controllers"});
            s->closeAfterFlush();
            return;
        }
    }
    s->info.name = hello.clientName.empty() ? "client-" + std::to_string(s->info.connectionId) : hello.clientName;
    s->info.role = hello.role;
    s->info.clientId = hello.clientId;
    s->meterPort = hello.meterPort;
    s->ready = true;
    ++readyClients_;

    Welcome w;
    w.proto = kProtocolVersion;
    w.serverName = options_.serverName;
    w.serverId = options_.serverId;
    w.sessionId = options_.sessionId;
    w.token = token;
    s->send(w);
    s->send(store_.snapshot());
    if (options_.onClient) options_.onClient(s->info, true);
}

const ControlServer::CallHandler* ControlServer::findHandler(const std::string& method) const {
    if (auto it = handlers_.find(method); it != handlers_.end()) return &it->second;
    if (auto it = handlers_.find(Schema::patternOf(method)); it != handlers_.end()) return &it->second;
    return nullptr;
}

void ControlServer::handleCall(const std::shared_ptr<Session>& s, const Call& call) {
    Result result;
    const CallHandler* handler = isValidPath(call.method) ? findHandler(call.method) : nullptr;
    const bool peerAllowed = call.method == "/lan/subscribe" || call.method == "/lan/unsubscribe" ||
                             call.method == "/session/resync";
    if (s->info.role == ClientRole::Peer && !peerAllowed) {
        result.code = ErrorCode::Unauthorized;
        result.message = "a peer engine may only subscribe";
    } else if (!handler) {
        result.code = ErrorCode::UnknownMethod;
        result.message = "unknown method " + call.method;
    } else {
        CallContext ctx{call, s->info, *this};
        try {
            result = (*handler)(ctx);
        } catch (const std::exception& e) {
            result = {};
            result.code = ErrorCode::Internal;
            result.message = e.what();
        }
    }
    result.id = call.id;
    if (call.method == "/session/resync" && result.code == ErrorCode::Ok) s->send(store_.snapshot());
    s->send(result);
}

Result ControlServer::applyLocalSet(const Set& set, const std::string& clientName) {
    auto outcome = store_.applySet(set, clientName);
    if (outcome.delta) emitDelta(*outcome.delta);
    return outcome.result;
}

Result ControlServer::callLocal(const Call& call, const ClientInfo& client) {
    const CallHandler* handler = isValidPath(call.method) ? findHandler(call.method) : nullptr;
    Result result;
    if (!handler) {
        result.code = ErrorCode::UnknownMethod;
        result.message = "unknown method " + call.method;
    } else {
        CallContext ctx{call, client, *this};
        try {
            result = (*handler)(ctx);
        } catch (const std::exception& e) {
            result = {};
            result.code = ErrorCode::Internal;
            result.message = e.what();
        }
    }
    result.id = call.id;
    return result;
}

void ControlServer::broadcast(const Message& msg) {
    auto frame = std::make_shared<Bytes>(encodeFrame(msg));
    auto copy = sessions_; // sending may close a session and modify the map
    for (auto& [id, s] : copy)
        if (s->ready) s->sendBytes(frame);
}

void ControlServer::publishNow(const std::vector<Change>& changes, const std::vector<std::string>& removed,
                               std::uint64_t originId, const std::string& originClient) {
    if (auto delta = store_.publish(changes, removed, originId, originClient)) emitDelta(*delta);
}

void ControlServer::emitDelta(const Delta& delta) {
    broadcast(delta);
    if (options_.onDelta) options_.onDelta(delta);
}

void ControlServer::broadcastEventNow(const Event& ev) { broadcast(ev); }

void ControlServer::publish(std::vector<Change> changes, std::vector<std::string> removed) {
    asio::post(io_, [this, c = std::move(changes), r = std::move(removed)] { publishNow(c, r); });
}

void ControlServer::broadcastEvent(Event ev) {
    asio::post(io_, [this, e = std::move(ev)] { broadcastEventNow(e); });
}

void ControlServer::sendMeters(MeterPacket packet) {
    asio::post(io_, [this, p = std::move(packet)] {
        if (!meterSocket_.is_open()) return;
        const auto bytes = encodeMeterPacket(p);
        for (const auto& [id, s] : sessions_) {
            if (!s->ready || s->meterPort == 0) continue;
            asio::error_code ec;
            meterSocket_.send_to(asio::buffer(bytes), asio::ip::udp::endpoint(s->remoteAddress, s->meterPort), 0, ec);
        }
    });
}

} // namespace dc::control
