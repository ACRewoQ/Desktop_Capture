#include "lan_link.hpp"

#include "dc/control/client.hpp"
#include "dc/lanmedia/video_link.hpp"
#include "dc/protocol/constants.hpp"
#include "dc/protocol/lan.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>

namespace v6 {

using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

void splitEngine(const std::string& s, std::string& host, int& port) {
    host = s.empty() ? "127.0.0.1" : s;
    port = dc::proto::kDefaultControlPort;
    if (const auto colon = host.rfind(':'); colon != std::string::npos) {
        port = std::atoi(host.c_str() + colon + 1);
        host = host.substr(0, colon);
    }
}

std::string computerName() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(buf, &n)) return "viewer";
    return toUtf8(buf);
}

// %APPDATA%\DesktopCapture\v6-viewer.txt: this viewer's client id and the token of each engine.
struct ViewerStore {
    std::filesystem::path path;
    std::string clientId;
    std::map<std::string, std::string> tokens;  // "host:port" -> token

    ViewerStore() {
        if (const wchar_t* a = _wgetenv(L"APPDATA")) path = std::filesystem::path(a) / L"DesktopCapture" / L"v6-viewer.txt";
        else path = L"v6-viewer.txt";
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream f(line);
            std::string kind, a, b;
            std::getline(f, kind, '\t');
            std::getline(f, a, '\t');
            std::getline(f, b, '\t');
            if (kind == "client_id") clientId = a;
            else if (kind == "token" && !b.empty()) tokens[a] = b;
        }
        if (clientId.empty()) {
            std::random_device rd;
            char hex[17];
            std::snprintf(hex, sizeof hex, "%08x%08x", rd(), rd());
            clientId = std::string("v6-") + hex;
            save();
        }
    }
    void save() const {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::trunc);
        out << "client_id\t" << clientId << "\n";
        for (const auto& [k, v] : tokens) out << "token\t" << k << "\t" << v << "\n";
    }
};

}  // namespace

// --- sender ----------------------------------------------------------------------------------------------------

LanVideoSender::LanVideoSender(const std::string& engine, std::string stream, std::uint16_t videoPort)
    : stream_(std::move(stream)) {
    splitEngine(engine, host_, port_);
    dc::lan::VideoOutput::Options o;
    o.port = videoPort;
    out_ = std::make_unique<dc::lan::VideoOutput>(o);
    thread_ = std::thread([this] { run(); });
}

LanVideoSender::~LanVideoSender() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void LanVideoSender::send(const EncodedUnit& unit) {
    if (!out_->hasDestinations()) return;
    out_->send(unit.data, unit.keyframe, dc::lan::nowSeconds());
}

bool LanVideoSender::keyframeWanted() const { return out_->keyframeWanted(); }

std::string LanVideoSender::status() const {
    const auto s = out_->stats();
    char buf[256];
    std::snprintf(buf, sizeof buf, "engine %s, %d viewer(s), sent %llu frames (%llu key), %.1f MB, keyframe requests %llu",
                  connected_ ? "connected" : "not connected", viewers_.load(),
                  static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.keyframes),
                  static_cast<double>(s.bytes) / 1e6, static_cast<unsigned long long>(s.keyframeRequests));
    return buf;
}

void LanVideoSender::run() {
    std::unique_ptr<dc::control::ControlClient> client;
    auto nextConnect = steady_clock::now();
    while (!stop_) {
        if (client && !client->connected()) {
            client.reset();
            connected_ = false;
            out_->setDestinations({});
            viewers_ = 0;
        }
        if (!client && steady_clock::now() >= nextConnect) {
            dc::control::ControlClient::Options o;
            o.host = host_;
            o.port = static_cast<std::uint16_t>(port_);
            o.clientName = "dc-v6-bench";
            o.role = dc::proto::ClientRole::Service;
            auto c = std::make_unique<dc::control::ControlClient>(o);
            std::string err;
            if (c->connect(2s, &err)) {
                client = std::move(c);
                connected_ = true;
            } else {
                std::cerr << "engine " << host_ << ":" << port_ << ": " << err << "\n";
                nextConnect = steady_clock::now() + 2s;
            }
        }
        if (client) {
            std::vector<dc::lan::UdpTransport::Endpoint> to;
            for (const auto& s : dc::proto::lanSubscriptions(client->tree(), stream_)) {
                asio::error_code ec;
                const auto addr = asio::ip::make_address_v4(s.host, ec);
                if (!ec) to.emplace_back(addr, s.port);
            }
            viewers_ = static_cast<int>(to.size());
            out_->setDestinations(std::move(to));
        }
        std::this_thread::sleep_for(100ms);
    }
}

// --- viewer ----------------------------------------------------------------------------------------------------

LanVideoViewer::LanVideoViewer(Options options, std::function<void(EncodedUnit&&)> onUnit)
    : o_(std::move(options)), onUnit_(std::move(onUnit)) {
    splitEngine(o_.engine, host_, port_);
    in_ = std::make_unique<dc::lan::VideoInput>(dc::lan::VideoInput::Options{}, [this](const dc::lan::VideoReceiver::Frame& f) {
        EncodedUnit u;
        u.id = f.number;
        u.keyframe = f.keyframe;
        u.data.assign(f.data.begin(), f.data.end());
        onUnit_(std::move(u));
    });
    thread_ = std::thread([this] { run(); });
}

LanVideoViewer::~LanVideoViewer() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    in_.reset();
}

std::string LanVideoViewer::status() const {
    const auto s = in_->stats();
    char buf[320];
    std::lock_guard lock(mu_);
    std::snprintf(buf, sizeof buf, "%s | %llu frames (%llu key), dropped %llu, lost datagrams %llu, keyframe requests %llu, jitter %.1f ms",
                  state_.c_str(), static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.keyframes),
                  static_cast<unsigned long long>(s.droppedFrames), static_cast<unsigned long long>(s.lostDatagrams),
                  static_cast<unsigned long long>(s.keyframeRequests), s.latenessPeakMs);
    return buf;
}

void LanVideoViewer::run() {
    ViewerStore store;
    const std::string key = host_ + ":" + std::to_string(port_);
    std::string pairCode = o_.pairCode;
    std::unique_ptr<dc::control::ControlClient> client;
    bool subscribed = false;
    auto setState = [&](std::string s) {
        std::lock_guard lock(mu_);
        if (s != state_) std::cout << "engine: " << s << std::endl;
        state_ = std::move(s);
    };
    auto nextConnect = steady_clock::now();
    while (!stop_) {
        if (client && !client->connected()) {
            client.reset();
            subscribed = false;
            setState("disconnected, retrying");
        }
        if (!client && steady_clock::now() >= nextConnect) {
            dc::control::ControlClient::Options o;
            o.host = host_;
            o.port = static_cast<std::uint16_t>(port_);
            o.clientName = "dc-v6-preview (" + computerName() + ")";
            o.clientId = store.clientId;
            o.role = dc::proto::ClientRole::Remote;
            if (auto t = store.tokens.find(key); t != store.tokens.end() && pairCode.empty()) o.token = t->second;
            else o.pairCode = pairCode;
            auto c = std::make_unique<dc::control::ControlClient>(o);
            std::string err;
            if (c->connect(3s, &err)) {
                if (!c->welcome().token.empty()) {
                    store.tokens[key] = c->welcome().token;
                    store.save();
                    pairCode.clear();
                }
                client = std::move(c);
                setState("connected to " + key);
            } else {
                setState(err.rfind("unauthorized", 0) == 0 ? "refused (" + err + "): start pairing on the engine and pass --pair CODE"
                                                           : "cannot reach " + key + ": " + err);
                nextConnect = steady_clock::now() + 3s;
            }
        }
        if (client && !subscribed) {
            auto f = client->call("/lan/subscribe", {{"stream", o_.stream}, {"port", std::int64_t{in_->localPort()}}});
            if (f.wait_for(3s) == std::future_status::ready) {
                const auto r = f.get();
                if (r.code == dc::proto::ErrorCode::Ok) {
                    subscribed = true;
                    setState("watching " + o_.stream + " from " + key);
                } else {
                    setState("subscribe failed: " + r.message);
                }
            }
        }
        std::this_thread::sleep_for(200ms);
    }
    if (client) client->disconnect();  // the engine drops the subscription with the connection
}

}  // namespace v6
