// The engine on the LAN (docs/spec/protocol.md 8, 10): two engines with fake devices in one process,
// one sending a bus as a LAN stream, the other playing it on a track; pairing of remote controllers
// and peer engines over a non-loopback address.

#include "controller.hpp"
#include "device_manager.hpp"
#include "engine.hpp"
#include "fake_backend.hpp"
#include "lan_service.hpp"
#include "lan_store.hpp"

#include "dc/control/client.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

using namespace dc;
using namespace dc::mixd;
using namespace std::chrono_literals;
using Catch::Approx;

namespace {

// One mixd: fake devices, engine, control server, controller and LAN service on a network thread.
struct Rig {
    explicit Rig(const std::string& bind = "127.0.0.1") {
        LanService::Options lo;
        lo.mediaPort = 0;
        lo.discovery = false;
        lo.verbose = false;
        lan = std::make_unique<LanService>(io, lo);

        control::ControlServer::Options opt;
        opt.bindAddress = bind;
        opt.port = 0;
        opt.serverId = lan->serverId();
        opt.authorize = [this](const proto::Hello& h, const control::ClientInfo& c) { return lan->authorize(h, c); };
        opt.onDelta = [this](const proto::Delta& d) {
            if (ready) ctl->onDelta(d);
        };
        opt.onClient = [this](const control::ClientInfo& c, bool connected) {
            if (ready) ctl->onClient(c, connected);
        };
        server = std::make_unique<control::ControlServer>(io, store, opt);
        devices = std::make_unique<DeviceManager>(backend, engine, DeviceManager::Options{});
        Controller::Options co;
        co.verbose = false;
        co.lan = lan.get();
        ctl = std::make_unique<Controller>(io, *server, *devices, engine, nullptr, co);
        server->start();
        ctl->start(devices->enumerateNow());
        lan->start(*server, server->port(), [this] { ctl->portsChanged(); });
        ready = true;
        devices->start([this](const DeviceManager::State& s) { ctl->onDeviceState(s); });
        net = std::thread([this] { io.run(); });
    }
    ~Rig() {
        asio::post(io, [this] {
            lan->stop();
            ctl->stop();
            server->stop();
            ready = false;
            work.reset();
            io.stop();
        });
        net.join();
        devices->stop();
    }

    std::unique_ptr<control::ControlClient> local() {
        control::ControlClient::Options o;
        o.port = server->port();
        o.clientName = "test";
        auto c = std::make_unique<control::ControlClient>(o);
        REQUIRE(c->connect(3s));
        return c;
    }

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work = asio::make_work_guard(io);
    proto::ParamStore store;
    FakeBackend backend;
    Engine engine{nullptr};
    std::unique_ptr<LanService> lan;
    std::unique_ptr<control::ControlServer> server;
    std::unique_ptr<DeviceManager> devices;
    std::unique_ptr<Controller> ctl;
    bool ready = false;
    std::thread net;
};

template <class Pred>
bool waitFor(control::ControlClient& c, Pred pred, std::chrono::milliseconds limit = 5000ms) {
    for (auto t = 0ms; t < limit; t += 50ms) {
        if (pred(c.tree())) return true;
        std::this_thread::sleep_for(50ms);
    }
    return pred(c.tree());
}

std::string str(const proto::ParamTree& t, const std::string& p) { return t.getAs<std::string>(p).value_or(""); }

proto::ErrorCode setOk(control::ControlClient& c, std::vector<proto::Change> changes) {
    return c.set(std::move(changes)).get().code;
}

// An address of this machine other than loopback (the server then sees a LAN client), if any.
std::optional<std::string> lanAddress() {
    try {
        asio::io_context io;
        asio::ip::udp::socket s(io);
        s.connect({asio::ip::make_address("192.0.2.1"), 9}); // no packet is sent
        const auto a = s.local_endpoint().address();
        if (a.is_v4() && !a.is_loopback() && !a.is_unspecified()) return a.to_string();
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("LAN store: server id, pairings, peer tokens and manual peers survive a restart", "[mixd][lan]") {
    const auto path = (std::filesystem::temp_directory_path() / "dc-test-mixd-lan.txt").string();
    std::filesystem::remove(path);
    std::string id, token;
    {
        LanStore s(path);
        s.load();
        id = s.serverId();
        CHECK(id.size() == 16);
        token = s.addPaired("phone-1", "Reo's\tphone", proto::ClientRole::Remote);
        CHECK(token.size() == 32);
        s.setPeerToken("peer-a", "tok-a");
        s.addManualPeer("192.168.1.20:47800");
        s.setManualPeerId("192.168.1.20:47800", "peer-a");
    }
    LanStore s(path);
    s.load();
    CHECK(s.serverId() == id);
    REQUIRE(s.findByToken(token));
    CHECK(s.findByToken(token)->id == "phone-1");
    CHECK(s.findByToken(token)->name == "Reo's phone"); // tabs cannot break the file
    CHECK(s.findByToken("") == nullptr);
    CHECK(s.peerTokens().at("peer-a") == "tok-a");
    REQUIRE(s.manualPeers().size() == 1);
    CHECK(s.manualPeers()[0].id == "peer-a");
    CHECK(s.removePaired("phone-1"));
    CHECK_FALSE(s.findByToken(token));
    std::filesystem::remove(path);

    PairingWindow w;
    const auto code = w.start(0);
    CHECK(code.size() == 6);
    CHECK(w.active(1));
    CHECK_FALSE(w.active(PairingWindow::kSeconds + 1));
    const std::string wrong = code == "000000" ? "111111" : "000000";
    for (int i = 0; i < PairingWindow::kMaxFailures - 1; ++i) CHECK(w.check(wrong, 1) == PairingWindow::Check::Wrong);
    CHECK(w.check(code, 1) == PairingWindow::Check::Ok);
    CHECK(w.check(code, 1) == PairingWindow::Check::Closed); // one client per window
    w.start(0);
    for (int i = 0; i < PairingWindow::kMaxFailures; ++i) w.check(wrong, 1);
    CHECK_FALSE(w.active(1)); // too many wrong codes close the window
}

TEST_CASE("two engines: a bus sent as a LAN stream plays on the other engine's track", "[mixd][lan][integration]") {
    Rig a, b;
    auto ca = a.local();
    auto cb = b.local();
    const auto idA = a.lan->serverId();
    REQUIRE(str(ca->tree(), "/lan/id") == idA);

    // the sender: the line tone through the master, which also goes to LAN stream 1
    REQUIRE(setOk(*ca, {{"/track/1/input/kind", std::string("device")},
                        {"/track/1/input/source", std::string("fake-line")},
                        {"/lan/stream/1/name", std::string("Game PC mix")},
                        {"/master/out/2/target", std::string("lan/1")}}) == proto::ErrorCode::Ok);
    CHECK(str(ca->tree(), "/lan/stream/1/codec") == "pcm24");

    // the receiver adds the sender by address and pairs (a loopback peer needs no code)
    const auto address = "127.0.0.1:" + std::to_string(a.server->port());
    REQUIRE(cb->call("/lan/peers/add", {{"address", address}}).get().code == proto::ErrorCode::Ok);
    REQUIRE(waitFor(*cb, [](const proto::ParamTree& t) { return str(t, "/lan/peers/1/state") == "unpaired"; }));
    const auto tmpId = str(cb->tree(), "/lan/peers/1/id");
    CHECK(tmpId == "addr:" + address);
    CHECK(cb->call("/lan/peers/add", {{"address", std::string("nope")}}).get().code == proto::ErrorCode::OutOfRange);
    REQUIRE(cb->call("/lan/peers/pair", {{"id", tmpId}, {"code", std::string("000000")}}).get().code ==
            proto::ErrorCode::Ok);
    REQUIRE(waitFor(*cb, [&](const proto::ParamTree& t) {
        return str(t, "/lan/peers/1/id") == idA && str(t, "/lan/peers/1/state") == "connected" &&
               str(t, "/lan/peers/1/stream/1/name") == "Game PC mix";
    }));
    CHECK(cb->tree().getAs<bool>("/lan/peers/1/manual") == true);

    // a track plays the stream: the peer link subscribes, the sender opens the stream
    REQUIRE(setOk(*cb, {{"/track/1/input/kind", std::string("lan")}, {"/track/1/input/source", idA + "/1"},
                        {"/track/1/channels", std::int64_t{2}}}) == proto::ErrorCode::Ok);
    REQUIRE(waitFor(*ca, [](const proto::ParamTree& t) {
        return t.getAs<std::int64_t>("/lan/stream/1/subscribers") == 1 && t.getAs<bool>("/lan/stream/1/active") == true &&
               t.getAs<std::int64_t>("/lan/stream/1/stream_id").value_or(0) != 0 && t.contains("/lan/sub/1/stream");
    }));
    CHECK(str(ca->tree(), "/lan/sub/1/stream") == "audio/1");
    CHECK(waitFor(*cb, [](const proto::ParamTree& t) { return str(t, "/track/1/input/state") == "ok"; }));
    CHECK(waitFor(*cb, [](const proto::ParamTree& t) {
        return t.getAs<double>("/track/1/input/lan_delay_ms").value_or(0) > 0;
    }));
    CHECK(waitFor(*cb, [](const proto::ParamTree& t) { return t.getAs<bool>("/lan/peers/1/stream/1/active") == true; }));

    // the tone arrives at the receiver's speakers at the level it left
    std::this_thread::sleep_for(1500ms);
    b.backend.takeOutputPeak("fake-spk");
    std::this_thread::sleep_for(500ms);
    CHECK(b.backend.takeOutputPeak("fake-spk") == Approx(0.25).margin(0.03));

    // a peer engine may only subscribe
    {
        control::ControlClient::Options o;
        o.port = a.server->port();
        o.clientName = "peer";
        o.role = proto::ClientRole::Peer;
        control::ControlClient peer(o);
        REQUIRE(peer.connect(3s));
        CHECK(setOk(peer, {{"/master/volume_db", -6.0}}) == proto::ErrorCode::Unauthorized);
        CHECK(peer.call("/mixer/track/add").get().code == proto::ErrorCode::Unauthorized);
        CHECK(peer.call("/lan/subscribe", {{"stream", std::string("audio/9")}, {"port", std::int64_t{5000}}}).get().code ==
              proto::ErrorCode::OutOfRange);
        auto r = peer.call("/lan/subscribe", {{"stream", std::string("audio/2")}, {"port", std::int64_t{5000}}}).get();
        REQUIRE(r.code == proto::ErrorCode::Ok);
        CHECK(waitFor(*ca, [](const proto::ParamTree& t) { return t.getAs<std::int64_t>("/lan/stream/2/subscribers") == 1; }));
        CHECK(ca->tree().getAs<bool>("/lan/stream/2/active") == false); // nothing routed to lan/2
        CHECK(peer.call("/lan/unsubscribe", {{"id", std::int64_t{1}}}).get().code == proto::ErrorCode::Unauthorized);
        // the connection goes away: its subscriptions with it
    }
    CHECK(waitFor(*ca, [](const proto::ParamTree& t) { return t.getAs<std::int64_t>("/lan/stream/2/subscribers") == 0; }));

    // the track stops using the stream: the subscription is dropped
    REQUIRE(setOk(*cb, {{"/track/1/input/kind", std::string("none")}}) == proto::ErrorCode::Ok);
    CHECK(waitFor(*ca, [](const proto::ParamTree& t) {
        return t.getAs<std::int64_t>("/lan/stream/1/subscribers") == 0 && t.getAs<bool>("/lan/stream/1/active") == false &&
               !t.contains("/lan/sub/1/stream");
    }));

    // forgetting the peer removes it (it was added by address)
    REQUIRE(cb->call("/lan/peers/forget", {{"id", idA}}).get().code == proto::ErrorCode::Ok);
    CHECK(waitFor(*cb, [](const proto::ParamTree& t) { return !t.contains("/lan/peers/1/id"); }));
}

TEST_CASE("pairing: LAN clients need a code once, then their token", "[mixd][lan][integration]") {
    const auto ip = lanAddress();
    if (!ip) SKIP("no non-loopback address on this machine");
    Rig a("0.0.0.0");
    auto ca = a.local();

    control::ControlClient::Options o;
    o.host = *ip;
    o.port = a.server->port();
    o.clientName = "phone";
    o.clientId = "phone-1";
    o.role = proto::ClientRole::Remote;
    std::string err;
    {
        control::ControlClient c(o);
        CHECK_FALSE(c.connect(2s, &err));
        CHECK(err.find("not paired") != std::string::npos);
    }
    {
        auto o2 = o;
        o2.role = proto::ClientRole::Local; // only remote controllers and peer engines come from the LAN
        o2.pairCode = "123456";
        control::ControlClient c(o2);
        CHECK_FALSE(c.connect(2s));
    }
    auto started = ca->call("/remote/pairing/start").get();
    REQUIRE(started.code == proto::ErrorCode::Ok);
    const auto code = std::get<std::string>(started.value);
    CHECK(waitFor(*ca, [](const proto::ParamTree& t) { return t.getAs<bool>("/remote/pairing/active") == true; }));
    {
        auto o2 = o;
        o2.pairCode = code == "000000" ? "111111" : "000000";
        control::ControlClient c(o2);
        CHECK_FALSE(c.connect(2s, &err));
        CHECK(err.find("wrong pairing code") != std::string::npos);
    }
    std::string token;
    {
        auto o2 = o;
        o2.pairCode = code;
        control::ControlClient c(o2);
        REQUIRE(c.connect(2s, &err));
        token = c.welcome().token;
        CHECK(token.size() == 32);
        CHECK(c.welcome().serverId == a.lan->serverId());
        CHECK(waitFor(*ca, [](const proto::ParamTree& t) {
            return str(t, "/remote/paired/1/id") == "phone-1" && str(t, "/remote/paired/1/role") == "remote" &&
                   t.getAs<bool>("/remote/paired/1/online") == true && t.getAs<std::int64_t>("/remote/clients") == 1 &&
                   t.getAs<bool>("/remote/pairing/active") == false;
        }));
    }
    CHECK(waitFor(*ca, [](const proto::ParamTree& t) {
        return t.getAs<bool>("/remote/paired/1/online") == false && t.getAs<std::int64_t>("/remote/clients") == 0;
    }));
    {
        auto o2 = o;
        o2.token = token;
        control::ControlClient c(o2);
        REQUIRE(c.connect(2s));
        CHECK(setOk(c, {{"/master/volume_db", -3.0}}) == proto::ErrorCode::Ok);
        // unpairing drops the connection and the token
        REQUIRE(ca->call("/remote/unpair", {{"id", std::string("phone-1")}}).get().code == proto::ErrorCode::Ok);
        for (int i = 0; i < 40 && c.connected(); ++i) std::this_thread::sleep_for(50ms);
        CHECK_FALSE(c.connected());
    }
    {
        auto o2 = o;
        o2.token = token;
        control::ControlClient c(o2);
        CHECK_FALSE(c.connect(2s, &err));
    }

    // a peer engine pairs the same way, through /lan/peers/pair, and then holds a link
    Rig b("0.0.0.0");
    auto cb = b.local();
    const auto address = *ip + ":" + std::to_string(a.server->port());
    REQUIRE(cb->call("/lan/peers/add", {{"address", address}}).get().code == proto::ErrorCode::Ok);
    REQUIRE(waitFor(*cb, [](const proto::ParamTree& t) { return str(t, "/lan/peers/1/state") == "unpaired"; }));
    CHECK(cb->call("/lan/peers/pair", {{"id", "addr:" + address}, {"code", std::string("123456")}}).get().code ==
          proto::ErrorCode::Unauthorized); // pairing is not open
    const auto code2 = std::get<std::string>(ca->call("/remote/pairing/start").get().value);
    REQUIRE(cb->call("/lan/peers/pair", {{"id", "addr:" + address}, {"code", code2}}).get().code == proto::ErrorCode::Ok);
    CHECK(waitFor(*cb, [&](const proto::ParamTree& t) {
        return str(t, "/lan/peers/1/id") == a.lan->serverId() && str(t, "/lan/peers/1/state") == "connected";
    }));
    CHECK(waitFor(*ca, [&](const proto::ParamTree& t) {
        return str(t, "/remote/paired/1/id") == b.lan->serverId() && str(t, "/remote/paired/1/role") == "peer" &&
               t.getAs<bool>("/remote/paired/1/online") == true && t.getAs<std::int64_t>("/remote/clients") == 0;
    }));
    // unpaired on the other side: the link is refused from then on
    REQUIRE(ca->call("/remote/unpair", {{"id", b.lan->serverId()}}).get().code == proto::ErrorCode::Ok);
    CHECK(waitFor(*cb, [](const proto::ParamTree& t) { return str(t, "/lan/peers/1/state") == "refused"; }, 8000ms));
}
