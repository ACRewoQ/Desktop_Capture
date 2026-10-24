// End-to-end: mock engine + control server on a real TCP socket, several clients.
// This is the G0 check "モックサーバーとテスト用クライアントがスナップショットと差分をやり取りできる".

#include "mock_engine.hpp"

#include "dc/control/client.hpp"
#include "dc/protocol/constants.hpp"
#include "dc/protocol/meter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

using namespace dc;
using namespace dc::proto;
using namespace std::chrono_literals;

namespace {

class Harness {
public:
    explicit Harness(bool meters = false) {
        control::ControlServer::Options opt;
        opt.port = 0;
        opt.serverName = "test-mock";
        opt.sessionId = 77;
        server_ = std::make_unique<control::ControlServer>(io_, store_, opt);
        mock::MockEngine::Options mo;
        mo.meters = meters;
        mo.meterInterval = 10ms;
        engine_ = std::make_unique<mock::MockEngine>(io_, *server_, mo);
        server_->start();
        engine_->start();
        work_.emplace(asio::make_work_guard(io_));
        thread_ = std::thread([this] { io_.run(); });
    }
    ~Harness() {
        engine_->stop();
        server_->stop();
        work_.reset();
        asio::post(io_, [this] { io_.stop(); });
        thread_.join();
    }
    std::uint16_t port() const { return server_->port(); }

    // Copy of the authoritative tree, taken on the server thread.
    ParamTree serverTree() {
        std::promise<ParamTree> p;
        asio::post(io_, [&] { p.set_value(store_.tree()); });
        return p.get_future().get();
    }

    control::ControlServer& server() { return *server_; }

private:
    asio::io_context io_;
    ParamStore store_;
    std::unique_ptr<control::ControlServer> server_;
    std::unique_ptr<mock::MockEngine> engine_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::thread thread_;
};

std::unique_ptr<control::ControlClient> connect(std::uint16_t port, const std::string& name,
                                                const std::function<void(control::ControlClient&)>& setup = {}) {
    control::ControlClient::Options o;
    o.port = port;
    o.clientName = name;
    auto c = std::make_unique<control::ControlClient>(o);
    if (setup) setup(*c); // callbacks must be set before connect()
    std::string err;
    INFO(err);
    REQUIRE(c->connect(3s, &err));
    return c;
}

} // namespace

TEST_CASE("clients receive the snapshot on connect", "[integration]") {
    Harness h;
    auto a = connect(h.port(), "a");
    CHECK(a->welcome().proto == kProtocolVersion);
    CHECK(a->welcome().serverName == "test-mock");
    CHECK(a->welcome().sessionId == 77);
    const auto tree = a->tree();
    CHECK(tree == h.serverTree());
    CHECK(tree.getAs<std::int64_t>("/mixer/track_count") == 4);
    CHECK(tree.getAs<std::string>("/track/1/name") == "Mic");
    CHECK(tree.contains("/track/4/out/8/target"));
    CHECK_FALSE(tree.contains("/track/5/position"));
}

TEST_CASE("a change by one client reaches every client as a delta", "[integration]") {
    Harness h;
    std::vector<Delta> seenByB;
    std::mutex m;
    auto a = connect(h.port(), "a");
    auto b = connect(h.port(), "b", [&](control::ControlClient& c) {
        c.onDelta = [&](const Delta& d) {
            std::lock_guard lock(m);
            seenByB.push_back(d);
        };
    });

    const auto rev = a->tree().rev();
    auto r = a->set({{"/track/2/volume_db", -12.5}, {"/track/2/pan", std::int64_t{1}}}).get();
    CHECK(r.code == ErrorCode::Ok);
    REQUIRE(b->waitForRev(rev + 1, 2s));
    REQUIRE(a->waitForRev(rev + 1, 2s));

    CHECK(b->tree().getAs<double>("/track/2/volume_db") == -12.5);
    CHECK(b->tree().getAs<double>("/track/2/pan") == 1.0);
    {
        std::lock_guard lock(m);
        REQUIRE(seenByB.size() == 1);
        CHECK(seenByB[0].originClient == "a");
        CHECK(seenByB[0].originId == r.id);
    }
    CHECK(a->tree() == h.serverTree());
    CHECK(b->tree() == h.serverTree());
}

TEST_CASE("rejected changes return an error and change nothing", "[integration]") {
    Harness h;
    auto a = connect(h.port(), "a");
    const auto before = a->tree();
    auto r1 = a->set({{"/track/1/volume_db", 99.0}}).get();
    CHECK(r1.code == ErrorCode::OutOfRange);
    CHECK(r1.path == "/track/1/volume_db");
    auto r2 = a->set({{"/track/1/out/2/target", std::string("track/1")}}).get();
    CHECK(r2.code == ErrorCode::Conflict);
    auto r3 = a->call("/no/such/method").get();
    CHECK(r3.code == ErrorCode::UnknownMethod);
    auto r4 = a->call("/track/1/fx/add", {{"type", std::string("chorus")}}).get();
    CHECK(r4.code == ErrorCode::NotFound);
    CHECK(h.serverTree() == before);
}

TEST_CASE("adding, moving and removing tracks is mirrored", "[integration]") {
    Harness h;
    auto a = connect(h.port(), "a");
    auto b = connect(h.port(), "b");

    auto add = a->call("/mixer/track/add", {{"channels", std::int64_t{1}}, {"name", std::string("Guest")},
                                            {"position", std::int64_t{1}}}).get();
    REQUIRE(add.code == ErrorCode::Ok);
    CHECK(add.value == Value{std::int64_t{5}});
    REQUIRE(b->waitForRev(h.serverTree().rev(), 2s));
    auto t = b->tree();
    CHECK(t.getAs<std::string>("/track/5/name") == "Guest");
    CHECK(t.getAs<std::int64_t>("/track/5/channels") == 1);
    CHECK(t.getAs<std::int64_t>("/track/5/position") == 1);
    CHECK(t.getAs<std::int64_t>("/track/1/position") == 2);
    CHECK(t.getAs<std::int64_t>("/mixer/track_count") == 5);

    REQUIRE(a->set({{"/track/1/out/2/target", std::string("track/3")}, {"/capture/audio/4/source", std::string("track/3")}})
                .get()
                .code == ErrorCode::Ok);
    REQUIRE(a->call("/mixer/track/move", {{"track", std::int64_t{5}}, {"position", std::int64_t{5}}}).get().code ==
            ErrorCode::Ok);
    REQUIRE(a->call("/mixer/track/remove", {{"track", std::int64_t{3}}}).get().code == ErrorCode::Ok);
    REQUIRE(b->waitForRev(h.serverTree().rev(), 2s));
    t = b->tree();
    CHECK_FALSE(t.contains("/track/3/position"));
    CHECK_FALSE(t.contains("/track/3/volume_db"));
    CHECK(t.getAs<std::string>("/track/1/out/2/target") == ""); // reference cleared
    CHECK(t.getAs<std::string>("/capture/audio/4/source") == "");
    CHECK(t.getAs<std::string>("/capture/audio/3/source") == "track/2"); // untouched
    CHECK(t.getAs<std::int64_t>("/track/5/position") == 4);
    CHECK(t.getAs<std::int64_t>("/mixer/track_count") == 4);
    CHECK(t == h.serverTree());

    // the freed number is reused
    auto again = a->call("/mixer/track/add").get();
    CHECK(again.value == Value{std::int64_t{3}});
}

TEST_CASE("the 65th track is refused", "[integration]") {
    Harness h;
    auto a = connect(h.port(), "a");
    for (int i = 0; i < 60; ++i) REQUIRE(a->call("/mixer/track/add").get().code == ErrorCode::Ok);
    CHECK(a->call("/mixer/track/add").get().code == ErrorCode::LimitReached);
}

TEST_CASE("recording start and stop update state and send events", "[integration]") {
    Harness h;
    std::vector<std::string> events;
    std::mutex m;
    auto a = connect(h.port(), "a");
    auto b = connect(h.port(), "b", [&](control::ControlClient& c) {
        c.onEvent = [&](const Event& e) {
            std::lock_guard lock(m);
            events.push_back(e.name);
        };
    });

    REQUIRE(a->call("/capture/record/start").get().code == ErrorCode::Ok);
    CHECK(a->call("/capture/record/start").get().code == ErrorCode::Conflict);
    REQUIRE(b->waitForRev(h.serverTree().rev(), 2s));
    CHECK(b->tree().getAs<std::string>("/capture/state") == "recording");
    std::this_thread::sleep_for(300ms);
    REQUIRE(a->call("/capture/record/stop").get().code == ErrorCode::Ok);
    REQUIRE(b->waitForRev(h.serverTree().rev(), 2s));
    CHECK(b->tree().getAs<std::string>("/capture/state") == "idle");
    CHECK(b->tree().getAs<std::int64_t>("/capture/elapsed_ms").value_or(0) >= 250);
    std::lock_guard lock(m);
    CHECK(events == std::vector<std::string>{"capture.started", "capture.stopped"});
}

TEST_CASE("resync sends a fresh snapshot", "[integration]") {
    Harness h;
    std::atomic<int> snapshots{0};
    auto a = connect(h.port(), "a", [&](control::ControlClient& c) {
        c.onSnapshot = [&](const Snapshot&) { ++snapshots; };
    });
    REQUIRE(snapshots.load() == 1);
    REQUIRE(a->call("/session/resync").get().code == ErrorCode::Ok);
    CHECK(snapshots.load() == 2); // the snapshot is sent before the Result
    CHECK(a->tree() == h.serverTree());
}

TEST_CASE("engine-side changes are broadcast", "[integration]") {
    Harness h;
    auto a = connect(h.port(), "a");
    const auto rev = a->tree().rev();
    h.server().publish({{"/track/1/input/state", std::string("disconnected")}});
    REQUIRE(a->waitForRev(rev + 1, 2s));
    CHECK(a->tree().getAs<std::string>("/track/1/input/state") == "disconnected");
}

TEST_CASE("a client with the wrong protocol version is refused", "[integration]") {
    Harness h;
    asio::io_context io;
    asio::ip::tcp::socket s(io);
    s.connect({asio::ip::make_address("127.0.0.1"), h.port()});
    Hello hello;
    hello.proto = kProtocolVersion + 1;
    const auto frame = encodeFrame(hello);
    asio::write(s, asio::buffer(frame));
    FrameDecoder dec;
    std::array<std::uint8_t, 4096> buf{};
    std::optional<Message> reply;
    while (!reply) {
        asio::error_code ec;
        const auto n = s.read_some(asio::buffer(buf), ec);
        REQUIRE_FALSE(ec);
        dec.feed({buf.data(), n});
        if (auto p = dec.next()) reply = decodePayload(*p).message;
    }
    REQUIRE(std::holds_alternative<Error>(*reply));
    CHECK(std::get<Error>(*reply).code == ErrorCode::UnsupportedVersion);
}

TEST_CASE("meters arrive over UDP", "[integration]") {
    Harness h(true);
    asio::io_context io;
    asio::ip::udp::socket sock(io, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    control::ControlClient::Options o;
    o.port = h.port();
    o.clientName = "meter";
    o.meterPort = sock.local_endpoint().port();
    control::ControlClient c(o);
    REQUIRE(c.connect(3s));

    std::array<std::uint8_t, 2048> buf{};
    asio::ip::udp::endpoint from;
    const auto n = sock.receive_from(asio::buffer(buf), from);
    auto p = decodeMeterPacket({buf.data(), n});
    REQUIRE(p.has_value());
    CHECK(p->entries.size() == 5); // master + 4 tracks
    CHECK(p->entries[0].meterId == 0);
}
