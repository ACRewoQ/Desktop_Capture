#include "lan_service.hpp"

#include "dc/control/client.hpp"
#include "dc/lanmedia/audio_receiver.hpp"
#include "dc/lanmedia/audio_sender.hpp"
#include "dc/protocol/routing.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iostream>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

namespace dc::mixd {

using namespace dc::proto;
using control::CallContext;

namespace {

Result error(ErrorCode code, std::string message) {
    Result r;
    r.code = code;
    r.message = std::move(message);
    return r;
}

std::optional<std::string> strArg(const Args& args, const char* key) {
    if (auto it = args.find(key); it != args.end())
        if (auto* v = std::get_if<std::string>(&it->second)) return *v;
    return std::nullopt;
}

// A pairing code or id typed on a command line may arrive as a number ("012345" -> 12345).
std::optional<std::string> textArg(const Args& args, const char* key, int digits = 0) {
    if (auto s = strArg(args, key)) return s;
    if (auto it = args.find(key); it != args.end())
        if (auto* v = std::get_if<std::int64_t>(&it->second); v && *v >= 0) {
            auto t = std::to_string(*v);
            if (static_cast<int>(t.size()) < digits) t.insert(0, std::size_t(digits) - t.size(), '0');
            return t;
        }
    return std::nullopt;
}

std::optional<std::int64_t> intArg(const Args& args, const char* key) {
    if (auto it = args.find(key); it != args.end())
        if (auto* v = std::get_if<std::int64_t>(&it->second)) return *v;
    return std::nullopt;
}

std::string str(const ParamTree& t, const std::string& path) { return t.getAs<std::string>(path).value_or(""); }

// "audio/<n>" -> n, 0 otherwise
int audioStreamOf(const std::string& stream) {
    if (stream.rfind("audio/", 0) != 0) return 0;
    const auto arg = std::string_view(stream).substr(6);
    int n = 0;
    auto [p, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), n);
    if (ec != std::errc() || p != arg.data() + arg.size() || n < 1 || n > kMaxLanStreams || arg[0] == '0') return 0;
    return n;
}

// "<peer>/<n>" -> (peer, n)
std::optional<std::pair<std::string, int>> lanSource(const std::string& source) {
    const auto slash = source.rfind('/');
    if (slash == std::string::npos || slash == 0) return std::nullopt;
    const int n = audioStreamOf("audio/" + source.substr(slash + 1));
    if (n == 0) return std::nullopt;
    return std::make_pair(source.substr(0, slash), n);
}

// "ip" or "ip:port" -> host, port (IPv4 only, like the rest of the LAN side)
std::optional<std::pair<std::string, std::uint16_t>> parseAddress(const std::string& a) {
    std::string host = a;
    std::uint16_t port = kDefaultControlPort;
    if (const auto colon = a.rfind(':'); colon != std::string::npos) {
        host = a.substr(0, colon);
        int p = 0;
        const auto ps = std::string_view(a).substr(colon + 1);
        auto [e, ec] = std::from_chars(ps.data(), ps.data() + ps.size(), p);
        if (ec != std::errc() || e != ps.data() + ps.size() || p < 1 || p > 65535) return std::nullopt;
        port = static_cast<std::uint16_t>(p);
    }
    asio::error_code ec;
    const auto addr = asio::ip::make_address_v4(host, ec);
    if (ec) return std::nullopt;
    return std::make_pair(addr.to_string(), port);
}

std::string endpointText(const lan::UdpTransport::Endpoint& e) {
    return e.address().to_string() + ":" + std::to_string(e.port());
}

std::string streamBase(int n) { return "/lan/stream/" + std::to_string(n); }

} // namespace

LanService::LanService(asio::io_context& io, Options options) : io_(io), opts_(std::move(options)), store_(opts_.storePath) {
    store_.load();
}

LanService::~LanService() { stop(); }

std::uint16_t LanService::mediaPort() const { return transport_ ? transport_->localPort() : 0; }

std::string LanService::instanceName() const {
    const auto name = server_ ? str(server_->store().tree(), "/lan/name") : std::string();
    return name.empty() ? computerName() : name;
}

void LanService::start(control::ControlServer& server, std::uint16_t controlPort, std::function<void()> portsChanged) {
    server_ = &server;
    controlPort_ = controlPort;
    portsChanged_ = std::move(portsChanged);

    server.registerMethod("/lan/subscribe", [this](CallContext& c) { return subscribe(c); });
    server.registerMethod("/lan/unsubscribe", [this](CallContext& c) { return unsubscribe(c); });
    server.registerMethod("/remote/pairing/start", [this](CallContext& c) { return pairingStart(c); });
    server.registerMethod("/remote/pairing/stop", [this](CallContext& c) { return pairingStop(c); });
    server.registerMethod("/remote/unpair", [this](CallContext& c) { return unpair(c); });
    server.registerMethod("/lan/peers/pair", [this](CallContext& c) { return peerPair(c); });
    server.registerMethod("/lan/peers/forget", [this](CallContext& c) { return peerForget(c); });
    server.registerMethod("/lan/peers/add", [this](CallContext& c) { return peerAdd(c); });

    server.publishNow({{"/lan/id", store_.serverId()}});
    publishPaired();

    lan::UdpTransport::Options to;
    to.port = opts_.mediaPort;
    try {
        transport_ = std::make_unique<lan::UdpTransport>(to);
        transport_->start([this](std::span<const std::uint8_t> d, const lan::UdpTransport::Endpoint& from, double t) {
            onDatagram(d, from, t);
        });
    } catch (const std::exception& e) {
        transport_.reset();
        std::cerr << "LAN media: cannot open UDP port " << opts_.mediaPort << ": " << e.what()
                  << " (LAN streams disabled)" << std::endl;
    }
#ifdef _WIN32
    timeBeginPeriod(1); // 1 ms pump period
#endif
    pumpRunning_ = true;
    pump_ = std::thread([this] { pumpLoop(); });

    if (opts_.discovery) {
        discovery_ = makeDiscovery();
        if (discovery_) {
            advertisedName_ = instanceName();
            std::string err;
            const bool ok = discovery_->start({advertisedName_, store_.serverId(), controlPort_},
                                              [this](std::vector<DiscoveredPeer> list) {
                                                  asio::post(io_, [this, l = std::move(list)]() mutable {
                                                      if (!stopping_) onDiscovered(std::move(l));
                                                  });
                                              },
                                              &err);
            if (!ok) {
                std::cerr << "LAN discovery: " << err << " (add peers by address)" << std::endl;
                discovery_.reset();
            }
        }
    }
    started_ = true;
    reconcilePeers();
    publishPeers();
    reconcileStreams();
    reconcileInputs();
    if (opts_.verbose && transport_)
        std::cout << "LAN: id " << store_.serverId() << ", media UDP " << transport_->localPort()
                  << (discovery_ ? ", mDNS on" : ", mDNS off") << std::endl;
}

void LanService::stop() {
    if (stopping_.exchange(true)) return;
    if (discovery_) discovery_->stop();
    for (auto& [id, p] : peers_) p.conn.reset(); // joins the link threads
    if (pumpRunning_.exchange(false) && pump_.joinable()) pump_.join();
    if (transport_) transport_->stop();
#ifdef _WIN32
    if (started_) timeEndPeriod(1);
#endif
}

void LanService::changed() {
    if (portsChanged_) portsChanged_();
}

// --- pairing -------------------------------------------------------------------------------------

control::AuthDecision LanService::authorize(const Hello& hello, const control::ClientInfo&) {
    control::AuthDecision d;
    if (!hello.token.empty()) {
        if (store_.findByToken(hello.token)) d.allowed = true;
        else d.message = "unknown token (pair again)";
        return d;
    }
    if (hello.pairCode.empty()) {
        d.message = "not paired";
        return d;
    }
    if (hello.clientId.empty()) {
        d.message = "pairing needs a client_id";
        return d;
    }
    const double now = lan::nowSeconds();
    switch (pairing_.check(hello.pairCode, now)) {
    case PairingWindow::Check::Ok:
        d.allowed = true;
        d.token = store_.addPaired(hello.clientId, hello.clientName, hello.role);
        if (opts_.verbose) std::cout << "LAN: paired with " << hello.clientName << " (" << hello.clientId << ")" << std::endl;
        break;
    case PairingWindow::Check::Wrong: d.message = "wrong pairing code"; break;
    case PairingWindow::Check::Closed: d.message = "pairing is not open on this PC"; break;
    }
    if (server_) {
        server_->publishNow({{"/remote/pairing/active", pairing_.active(now)}});
        if (d.allowed) publishPaired();
    }
    return d;
}

Result LanService::pairingStart(CallContext&) {
    const auto code = pairing_.start(lan::nowSeconds());
    server_->publishNow({{"/remote/pairing/active", true}});
    Result r;
    r.value = code;
    return r;
}

Result LanService::pairingStop(CallContext&) {
    pairing_.stop();
    server_->publishNow({{"/remote/pairing/active", false}});
    return {};
}

Result LanService::unpair(CallContext& ctx) {
    const auto id = textArg(ctx.call.args, "id");
    if (!id || !store_.removePaired(*id)) return error(ErrorCode::NotFound, "not paired");
    server_->disconnectIf([&](const control::ClientInfo& c) { return c.clientId == *id && !c.loopback; }, "unpaired");
    publishPaired();
    return {};
}

void LanService::publishPaired() {
    if (!server_) return;
    std::set<std::string> online;
    for (const auto& c : server_->clients())
        if (!c.clientId.empty()) online.insert(c.clientId);
    std::vector<Change> c;
    int i = 0;
    for (const auto& p : store_.paired()) {
        if (++i > kMaxPairedClients) break;
        const auto base = "/remote/paired/" + std::to_string(i);
        c.push_back({base + "/id", p.id});
        c.push_back({base + "/name", p.name});
        c.push_back({base + "/role", std::string(p.role == ClientRole::Peer ? "peer" : "remote")});
        c.push_back({base + "/online", online.count(p.id) > 0});
    }
    // only replace the list when it changed (an unchanged list publishes nothing)
    const auto& tree = server_->store().tree();
    bool same = true;
    for (const auto& x : c)
        if (const auto* v = tree.get(x.path); !v || !(*v == x.value)) same = false;
    if (tree.contains("/remote/paired/" + std::to_string(i + 1) + "/id")) same = false;
    if (!same) server_->publishNow(c, {"/remote/paired"});
}

// --- subscriptions ---------------------------------------------------------------------------------

Result LanService::subscribe(CallContext& ctx) {
    const auto stream = strArg(ctx.call.args, "stream");
    const auto port = intArg(ctx.call.args, "port");
    if (!stream || !port) return error(ErrorCode::BadRequest, "stream and port are required");
    if (audioStreamOf(*stream) == 0 && *stream != "video/preview" && *stream != "video/lan")
        return error(ErrorCode::OutOfRange, "stream must be audio/1..8, video/preview or video/lan");
    if (*port < 1 || *port > 65535) return error(ErrorCode::OutOfRange, "port out of range");
    asio::error_code ec;
    auto addr = asio::ip::make_address(ctx.client.address, ec);
    if (ec) return error(ErrorCode::Internal, "no client address");
    if (addr.is_v6() && addr.to_v6().is_v4_mapped()) addr = asio::ip::make_address_v4(asio::ip::v4_mapped, addr.to_v6());
    const lan::UdpTransport::Endpoint ep(addr, static_cast<std::uint16_t>(*port));

    for (const auto& s : subs_)
        if (s.connection == ctx.client.connectionId && s.stream == *stream && s.endpoint == ep) {
            Result r;
            r.value = std::int64_t{s.id};
            return r;
        }
    int id = 0;
    for (int i = 1; i <= kMaxLanSubscriptions && id == 0; ++i)
        if (std::none_of(subs_.begin(), subs_.end(), [&](const Subscription& s) { return s.id == i; })) id = i;
    if (id == 0) return error(ErrorCode::LimitReached, "too many subscriptions");

    subs_.push_back({id, *stream, ep, ctx.client.connectionId, ctx.client.name});
    const auto base = "/lan/sub/" + std::to_string(id);
    server_->publishNow({{base + "/stream", *stream}, {base + "/address", endpointText(ep)}, {base + "/client", ctx.client.name}},
                        {}, ctx.call.id, ctx.client.name);
    if (opts_.verbose) std::cout << "LAN: " << ctx.client.name << " subscribed " << *stream << " -> " << endpointText(ep) << std::endl;
    reconcileStreams();
    Result r;
    r.value = std::int64_t{id};
    return r;
}

Result LanService::unsubscribe(CallContext& ctx) {
    const auto id = intArg(ctx.call.args, "id");
    auto it = std::find_if(subs_.begin(), subs_.end(), [&](const Subscription& s) { return id && s.id == *id; });
    if (it == subs_.end()) return error(ErrorCode::NotFound, "no such subscription");
    if (it->connection != ctx.client.connectionId && ctx.client.role != ClientRole::Local)
        return error(ErrorCode::Unauthorized, "not your subscription");
    removeSubscription(it->id);
    reconcileStreams();
    return {};
}

void LanService::removeSubscription(int id) {
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [&](const Subscription& s) { return s.id == id; }), subs_.end());
    server_->publishNow({}, {"/lan/sub/" + std::to_string(id)});
}

void LanService::onClient(const control::ClientInfo& client, bool connected) {
    if (!server_) return;
    if (!connected) {
        std::vector<int> gone;
        for (const auto& s : subs_)
            if (s.connection == client.connectionId) gone.push_back(s.id);
        for (int id : gone) removeSubscription(id);
        if (!gone.empty()) reconcileStreams();
    }
    std::int64_t remotes = 0;
    for (const auto& c : server_->clients())
        if (c.role == ClientRole::Remote && (connected || c.connectionId != client.connectionId)) ++remotes;
    server_->publishNow({{"/remote/clients", std::min<std::int64_t>(remotes, 4)}});
    publishPaired();
}

// --- send streams --------------------------------------------------------------------------------

void LanService::reconcileStreams() {
    if (!server_ || !started_) return;
    const auto& tree = server_->store().tree();
    std::set<int> routed;
    for (const auto& [path, v] : tree.values()) {
        if (path.size() < 7 || path.compare(path.size() - 7, 7, "/target") != 0) continue;
        const auto* s = std::get_if<std::string>(&v);
        if (!s) continue;
        if (auto t = parseTarget(*s); t && t->kind == RouteTarget::Kind::Lan)
            if (int n = lanStreamOf(*t); n > 0) routed.insert(n);
    }

    bool portsDiffer = false;
    std::vector<Change> c;
    for (int n = 1; n <= kMaxLanStreams; ++n) {
        auto& st = streams_[std::size_t(n - 1)];
        const auto base = streamBase(n);
        StreamConfig cfg;
        cfg.codec = tree.getAs<std::string>(base + "/codec").value_or("pcm24");
        cfg.opusFrames = static_cast<int>(tree.getAs<std::int64_t>(base + "/opus_frames").value_or(480));
        cfg.bitrateKbps = static_cast<int>(tree.getAs<std::int64_t>(base + "/bitrate_kbps").value_or(128));

        std::vector<lan::UdpTransport::Endpoint> to;
        for (const auto& s : subs_)
            if (audioStreamOf(s.stream) == n && std::find(to.begin(), to.end(), s.endpoint) == to.end()) to.push_back(s.endpoint);

        const bool want = !to.empty() && transport_;
        if (want && (!st.port || !(cfg == st.config))) {
            lan::AudioSender::Config sc;
            sc.encoder.channels = 2;
            if (cfg.codec == "opus") {
                sc.encoder.codec = lan::AudioCodec::Opus;
                sc.encoder.frames = static_cast<std::size_t>(cfg.opusFrames);
                sc.encoder.opusBitrate = cfg.bitrateKbps * 1000;
            } else {
                sc.encoder.codec = cfg.codec == "pcm16" ? lan::AudioCodec::Pcm16 : lan::AudioCodec::Pcm24;
                sc.encoder.frames = 120;
            }
            auto port = std::make_shared<OutputPort>("lan:" + std::to_string(n), clocksync::ClockBridge::Config{});
            port->lan = std::make_shared<lan::AudioSender>(sc);
            port->streaming = true;
            st.port = std::move(port);
            st.config = cfg;
            portsDiffer = true;
        } else if (!want && st.port) {
            st.port.reset();
            portsDiffer = true;
        }
        st.to = std::move(to);
        c.push_back({base + "/subscribers", static_cast<std::int64_t>(st.to.size())});
        c.push_back({base + "/stream_id", std::int64_t{st.port ? st.port->lan->streamId() : 0u}});
        c.push_back({base + "/active", st.port != nullptr && routed.count(n) > 0});
    }
    server_->publishNow(c);
    publishMedia();
    if (portsDiffer) changed();
}

void LanService::addPorts(PortTable& ports) const {
    for (int n = 1; n <= kMaxLanStreams; ++n)
        if (const auto& p = streams_[std::size_t(n - 1)].port) ports.outputs[p->id] = p;
    for (const auto& [key, in] : inputs_)
        if (in.port) ports.inputs[key] = in.port;
}

void LanService::publishMedia() {
    std::vector<MediaSend> send;
    for (const auto& st : streams_)
        if (st.port) send.push_back({st.port->lan, st.to});
    std::lock_guard lock(mediaMutex_);
    std::vector<MediaRecv> recv;
    for (const auto& [key, in] : inputs_) {
        if (!in.port) continue;
        MediaRecv r;
        r.streamId = in.streamId;
        r.receiver = in.port->lan;
        asio::error_code ec;
        r.peer = asio::ip::make_address(in.host, ec);
        for (const auto& old : mediaRecv_)
            if (old.receiver == r.receiver) {
                r.haveSource = old.haveSource;
                r.source = old.source;
            }
        recv.push_back(std::move(r));
    }
    mediaSend_ = std::move(send);
    mediaRecv_ = std::move(recv);
}

// --- receive -------------------------------------------------------------------------------------

void LanService::reconcileInputs() {
    if (!server_ || !started_) return;
    const auto& tree = server_->store().tree();
    struct Want {
        std::string peer;
        int n;
        double targetMs;
    };
    std::map<std::string, Want> want;
    for (int t = 1; t <= kMaxTracks; ++t) {
        const auto base = "/track/" + std::to_string(t);
        if (!tree.contains(base + "/position") || str(tree, base + "/input/kind") != "lan") continue;
        const auto src = lanSource(str(tree, base + "/input/source"));
        if (!src) continue;
        const auto key = "lan:" + src->first + "/" + std::to_string(src->second);
        if (want.count(key)) continue;
        const double target = tree.getAs<double>(base + "/input/lan_buffer_ms").value_or(0.0);
        want[key] = {src->first, src->second, target};
    }

    bool portsDiffer = false;
    std::map<std::string, std::set<int>> perPeer;
    for (const auto& [key, w] : want) {
        perPeer[w.peer].insert(w.n);
        const Peer* p = findPeer(w.peer);
        const std::uint32_t sid = p && p->link.connected ? p->link.streams[std::size_t(w.n - 1)].streamId : 0;
        auto it = inputs_.find(key);
        if (sid == 0 || !transport_) {
            if (it != inputs_.end() && it->second.port) portsDiffer = true;
            if (it != inputs_.end()) inputs_.erase(it);
            continue;
        }
        if (it != inputs_.end() && it->second.port && it->second.streamId == sid && it->second.targetMs == w.targetMs) continue;
        lan::AudioReceiver::Config rc;
        rc.channels = 2;
        rc.streamId = sid;
        rc.targetMs = w.targetMs;
        LanInput in;
        in.peer = w.peer;
        in.stream = w.n;
        in.targetMs = w.targetMs;
        in.streamId = sid;
        in.host = p->host;
        in.port = std::make_shared<InputPort>(key, 2, clocksync::ClockBridge::Config{});
        in.port->lan = std::make_shared<lan::AudioReceiver>(rc);
        inputs_[key] = std::move(in);
        portsDiffer = true;
    }
    for (auto it = inputs_.begin(); it != inputs_.end();) {
        if (want.count(it->first)) {
            ++it;
            continue;
        }
        it = inputs_.erase(it);
        portsDiffer = true;
    }
    for (auto& [id, p] : peers_)
        if (p.conn) p.conn->setWanted(perPeer.count(id) ? perPeer[id] : std::set<int>{});
    publishMedia();
    if (portsDiffer) changed();
}

void LanService::onDatagram(std::span<const std::uint8_t> d, const lan::UdpTransport::Endpoint& from, double arrival) {
    const auto type = lan::peekType(d);
    if (!type) return;
    std::lock_guard lock(mediaMutex_);
    if (*type == lan::PacketType::Report) {
        const auto r = lan::parseReport(d);
        if (!r) return;
        for (auto& s : mediaSend_)
            if (s.sender->streamId() == r->streamId) s.sender->onReport(*r, arrival);
        return;
    }
    if (d.size() < lan::kMediaHeaderBytes) return;
    const auto media = lan::parseMedia(d);
    if (!media || media->header.kind != lan::MediaKind::Audio) return;
    for (auto& r : mediaRecv_) {
        if (r.streamId != media->header.streamId || from.address() != r.peer) continue;
        r.receiver->onPacket(d, arrival);
        r.source = from;
        r.haveSource = true;
        return;
    }
}

void LanService::pumpLoop() {
    double nextReport = 0;
    while (pumpRunning_) {
        {
            std::lock_guard lock(mediaMutex_);
            for (auto& s : mediaSend_)
                s.sender->pump([&](std::span<const std::uint8_t> dg) {
                    for (const auto& ep : s.to) transport_->send(dg, ep);
                });
            const double now = lan::nowSeconds();
            if (now >= nextReport) {
                nextReport = now + 1.0;
                for (auto& r : mediaRecv_)
                    if (r.haveSource) transport_->send(lan::encodeReport(r.receiver->makeReport(now)), r.source);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// --- peers ---------------------------------------------------------------------------------------

LanService::Peer* LanService::findPeer(const std::string& id) {
    if (auto it = peers_.find(id); it != peers_.end()) return &it->second;
    return nullptr;
}

void LanService::onDiscovered(std::vector<DiscoveredPeer> list) {
    discovered_ = std::move(list);
    reconcilePeers();
    publishPeers();
    reconcileInputs();
}

void LanService::reconcilePeers() {
    struct Seen {
        std::string name, host;
        std::uint16_t port = 0;
        bool manual = false, discovered = false;
    };
    std::map<std::string, Seen> seen;
    for (const auto& d : discovered_) seen[d.id] = {d.name, d.host, d.port, false, true};
    for (const auto& m : store_.manualPeers()) {
        const auto a = parseAddress(m.address);
        if (!a) continue;
        const auto key = m.id.empty() ? "addr:" + m.address : m.id;
        auto& s = seen[key];
        s.manual = true;
        if (s.host.empty()) {
            s.host = a->first;
            s.port = a->second;
        }
    }
    for (const auto& [id, token] : store_.peerTokens()) seen.try_emplace(id);

    for (auto it = peers_.begin(); it != peers_.end();) {
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        it = peers_.erase(it); // joins its link
    }
    for (const auto& [id, s] : seen) {
        auto& p = peers_[id];
        p.id = id;
        if (!s.name.empty()) p.name = s.name;
        p.manual = s.manual;
        p.discovered = s.discovered;
        const auto token = store_.peerTokens().find(id);
        const bool paired = token != store_.peerTokens().end();
        const bool hostChanged = p.host != s.host || p.port != s.port;
        p.host = s.host;
        p.port = s.port;
        const bool wantLink = paired && !p.host.empty() && id != store_.serverId() && transport_;
        if (p.conn && (!wantLink || hostChanged || p.conn->options().token != token->second)) {
            p.conn.reset();
            p.link = {};
        }
        if (wantLink && !p.conn) {
            PeerLink::Options o;
            o.host = p.host;
            o.port = p.port;
            o.selfId = store_.serverId();
            o.selfName = instanceName();
            o.token = token->second;
            o.mediaPort = transport_->localPort();
            const std::string key = id;
            p.conn = std::make_unique<PeerLink>(o, [this, key](const PeerLinkStatus& st) {
                asio::post(io_, [this, key, st] {
                    if (!stopping_) onLinkStatus(key, st);
                });
            });
        }
    }
}

void LanService::onLinkStatus(const std::string& key, const PeerLinkStatus& status) {
    Peer* p = findPeer(key);
    if (!p || !p->conn) return;
    const bool namesChanged = !(p->link.streams == status.streams) || p->link.connected != status.connected ||
                              p->link.refused != status.refused || p->link.serverName != status.serverName;
    const bool justConnected = status.connected && !p->link.connected;
    p->link = status;
    if (!status.serverName.empty()) p->name = status.serverName;
    if (justConnected && opts_.verbose)
        std::cout << "LAN: connected to peer " << (p->name.empty() ? p->host : p->name) << " (" << key << ")" << std::endl;
    if (namesChanged) publishPeers();
    reconcileInputs();
}

void LanService::publishPeers() {
    if (!server_) return;
    std::vector<const Peer*> list;
    for (const auto& [id, p] : peers_) list.push_back(&p);
    std::sort(list.begin(), list.end(), [](const Peer* a, const Peer* b) { return a->name < b->name; });
    std::vector<Change> c;
    int i = 0;
    for (const Peer* p : list) {
        if (++i > kMaxLanPeers) break;
        const auto base = "/lan/peers/" + std::to_string(i);
        const bool paired = store_.peerTokens().count(p->id) > 0;
        std::string state = "offline";
        if (p->conn && p->link.connected) state = "connected";
        else if (p->conn && p->link.refused) state = "refused";
        else if (p->conn) state = "connecting";
        else if (!paired && !p->host.empty()) state = "unpaired";
        c.push_back({base + "/id", p->id});
        c.push_back({base + "/name", p->name.empty() ? p->host : p->name});
        c.push_back({base + "/address", p->host.empty() ? std::string() : p->host + ":" + std::to_string(p->port)});
        c.push_back({base + "/state", state});
        c.push_back({base + "/manual", p->manual});
        if (p->link.connected)
            for (int n = 1; n <= kMaxLanStreams; ++n) {
                const auto& s = p->link.streams[std::size_t(n - 1)];
                const auto sb = base + "/stream/" + std::to_string(n);
                c.push_back({sb + "/name", s.name.empty() ? "LAN " + std::to_string(n) : s.name});
                c.push_back({sb + "/active", s.active});
            }
    }
    server_->publishNow(c, {"/lan/peers"});
}

Result LanService::peerPair(CallContext& ctx) {
    const auto id = textArg(ctx.call.args, "id");
    const auto code = textArg(ctx.call.args, "code", 6);
    if (!id || !code) return error(ErrorCode::BadRequest, "id and code are required");
    Peer* p = findPeer(*id);
    if (!p || p->host.empty()) return error(ErrorCode::NotFound, "no such peer (or its address is unknown)");

    // Blocks the network thread for the short pairing handshake (a user action, at most 3 s).
    control::ControlClient::Options o;
    o.host = p->host;
    o.port = p->port;
    o.clientName = instanceName();
    o.clientId = store_.serverId();
    o.role = ClientRole::Peer;
    o.pairCode = *code;
    control::ControlClient client(o);
    std::string err;
    if (!client.connect(std::chrono::milliseconds(3000), &err)) {
        if (err.rfind("unauthorized", 0) == 0) return error(ErrorCode::Unauthorized, err);
        return error(ErrorCode::Internal, "cannot reach " + p->host + ": " + err);
    }
    const auto w = client.welcome();
    client.disconnect();
    if (w.serverId.empty() || w.serverId == store_.serverId()) return error(ErrorCode::Conflict, "that is this PC");
    store_.setPeerToken(w.serverId, w.token);
    if (p->manual)
        for (const auto& m : store_.manualPeers())
            if (const auto a = parseAddress(m.address); a && a->first == p->host && a->second == p->port)
                store_.setManualPeerId(m.address, w.serverId);
    if (opts_.verbose) std::cout << "LAN: paired with peer " << w.serverId << " at " << p->host << std::endl;
    reconcilePeers();
    publishPeers();
    return {};
}

Result LanService::peerForget(CallContext& ctx) {
    const auto id = textArg(ctx.call.args, "id");
    Peer* p = id ? findPeer(*id) : nullptr;
    if (!p) return error(ErrorCode::NotFound, "no such peer");
    store_.removePeerToken(*id);
    if (p->manual)
        for (const auto& m : std::vector<LanStore::ManualPeer>(store_.manualPeers()))
            if (m.id == *id || "addr:" + m.address == *id) store_.removeManualPeer(m.address);
    reconcilePeers();
    publishPeers();
    reconcileInputs();
    return {};
}

Result LanService::peerAdd(CallContext& ctx) {
    const auto address = strArg(ctx.call.args, "address");
    const auto a = address ? parseAddress(*address) : std::nullopt;
    if (!a) return error(ErrorCode::OutOfRange, "address must be an IPv4 address, optionally with :port");
    store_.addManualPeer(a->first + ":" + std::to_string(a->second));
    reconcilePeers();
    publishPeers();
    return {};
}

// --- tree changes and periodic state ----------------------------------------------------------------

void LanService::onDelta(const Delta& delta) {
    if (!started_) return;
    bool streams = false, inputs = false, name = false;
    auto look = [&](const std::string& path) {
        if (path.rfind("/lan/stream/", 0) == 0) {
            // the engine's own reports do not reconfigure anything
            for (const char* ro : {"/subscribers", "/active", "/stream_id"})
                if (path.size() > std::strlen(ro) && path.compare(path.size() - std::strlen(ro), std::strlen(ro), ro) == 0)
                    return;
            streams = true;
        } else if (path.rfind("/track/", 0) == 0) {
            if (path.find("/input/") != std::string::npos && path.find("/input/state") == std::string::npos &&
                path.find("/input/lan_delay_ms") == std::string::npos)
                inputs = true;
            if (path.size() > 7 && path.compare(path.size() - 7, 7, "/target") == 0) streams = true;
        } else if (path.rfind("/master/out/", 0) == 0) {
            streams = true;
        } else if (path == "/lan/name") {
            name = true;
        }
    };
    for (const auto& c : delta.changes) look(c.path);
    for (const auto& r : delta.removed)
        if (r.rfind("/track/", 0) == 0) inputs = streams = true;
    if (streams) reconcileStreams();
    if (inputs) reconcileInputs();
    if (name && discovery_) {
        const auto n = instanceName();
        if (n != advertisedName_) {
            advertisedName_ = n;
            discovery_->rename(n);
        }
    }
}

void LanService::tick() {
    if (!started_) return;
    const auto& tree = server_->store().tree();
    const double now = lan::nowSeconds();
    if (tree.getAs<bool>("/remote/pairing/active").value_or(false) && !pairing_.active(now))
        server_->publishNow({{"/remote/pairing/active", false}});

    bool stateChanged = false;
    std::map<std::string, double> delay;
    for (auto& [key, in] : inputs_) {
        if (!in.port) continue;
        const auto st = in.port->lan->stats();
        const bool playing = st.state == lan::AudioReceiver::State::Running;
        if (in.port->streaming.exchange(playing) != playing) stateChanged = true;
        delay[key.substr(4)] = playing ? std::round(st.fillMs * 10.0) / 10.0 : 0.0;
    }
    std::vector<Change> c;
    for (int t = 1; t <= kMaxTracks; ++t) {
        const auto base = "/track/" + std::to_string(t);
        if (!tree.contains(base + "/position") || str(tree, base + "/input/kind") != "lan") continue;
        const auto it = delay.find(str(tree, base + "/input/source"));
        c.push_back({base + "/input/lan_delay_ms", it != delay.end() ? it->second : 0.0});
    }
    if (!c.empty()) server_->publishNow(c);
    if (stateChanged) changed(); // the plan builder reads the ports' streaming flag into /input/state
}

} // namespace dc::mixd
