#include "lan_peer_link.hpp"

#include "dc/control/client.hpp"

#include <chrono>
#include <map>
#include <memory>

namespace dc::mixd {

using namespace std::chrono;

PeerLink::PeerLink(Options options, Update update) : opts_(std::move(options)), update_(std::move(update)) {
    thread_ = std::thread([this] { run(); });
}

PeerLink::~PeerLink() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void PeerLink::setWanted(std::set<int> streams) {
    {
        std::lock_guard lock(mutex_);
        if (streams == wanted_) return;
        wanted_ = std::move(streams);
    }
    cv_.notify_all();
}

void PeerLink::run() {
    PeerLinkStatus status, reported;
    bool first = true;
    auto report = [&] {
        if (first || !(status == reported)) {
            first = false;
            reported = status;
            if (update_) update_(status);
        }
    };

    std::unique_ptr<control::ControlClient> client;
    std::map<int, std::int64_t> subs; // stream -> subscription id on the peer
    auto nextConnect = steady_clock::now();

    while (true) {
        {
            std::unique_lock lock(mutex_);
            if (stop_) break;
        }
        if (client && !client->connected()) {
            client.reset();
            subs.clear();
            status.connected = false;
            status.subscribed.clear();
            status.streams = {};
            report();
        }
        if (!client && steady_clock::now() >= nextConnect) {
            control::ControlClient::Options o;
            o.host = opts_.host;
            o.port = opts_.port;
            o.clientName = opts_.selfName;
            o.clientId = opts_.selfId;
            o.role = proto::ClientRole::Peer;
            o.token = opts_.token;
            auto c = std::make_unique<control::ControlClient>(o);
            c->onSnapshot = [this](const proto::Snapshot&) { dirty_ = true; };
            c->onDelta = [this](const proto::Delta&) { dirty_ = true; };
            c->onDisconnected = [this](const std::string&) {
                dirty_ = true;
                cv_.notify_all();
            };
            std::string err;
            if (c->connect(milliseconds(2000), &err)) {
                client = std::move(c);
                status.connected = true;
                status.refused = false;
                status.serverId = client->welcome().serverId;
                dirty_ = true;
                nextConnect = steady_clock::now() + seconds(3);
            } else {
                status.refused = err.rfind("unauthorized", 0) == 0;
                // a peer that refused us will not change its mind soon
                nextConnect = steady_clock::now() + (status.refused ? seconds(15) : seconds(3));
                report();
            }
        }

        if (client) {
            if (dirty_.exchange(false)) {
                const auto tree = client->tree();
                status.serverName = tree.getAs<std::string>("/lan/name").value_or("");
                for (int n = 1; n <= proto::kMaxLanStreams; ++n) {
                    const auto base = "/lan/stream/" + std::to_string(n);
                    auto& s = status.streams[std::size_t(n - 1)];
                    s.name = tree.getAs<std::string>(base + "/name").value_or("");
                    s.active = tree.getAs<bool>(base + "/active").value_or(false);
                    s.streamId = static_cast<std::uint32_t>(tree.getAs<std::int64_t>(base + "/stream_id").value_or(0));
                }
            }
            std::set<int> wanted;
            {
                std::lock_guard lock(mutex_);
                wanted = wanted_;
            }
            for (int n : wanted) {
                if (subs.count(n)) continue;
                auto f = client->call("/lan/subscribe", {{"stream", "audio/" + std::to_string(n)},
                                                         {"port", std::int64_t{opts_.mediaPort}}});
                if (f.wait_for(seconds(2)) != std::future_status::ready) break;
                const auto r = f.get();
                if (r.code != proto::ErrorCode::Ok) break;
                if (auto* id = std::get_if<std::int64_t>(&r.value)) subs[n] = *id;
            }
            for (auto it = subs.begin(); it != subs.end();) {
                if (wanted.count(it->first)) {
                    ++it;
                    continue;
                }
                auto f = client->call("/lan/unsubscribe", {{"id", it->second}});
                f.wait_for(seconds(2));
                it = subs.erase(it);
            }
            status.subscribed.clear();
            for (const auto& [n, id] : subs) status.subscribed.insert(n);
            report();
        }

        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, milliseconds(100), [&] { return stop_; });
    }
    if (client) client->disconnect(); // the peer drops our subscriptions with the connection
}

} // namespace dc::mixd
