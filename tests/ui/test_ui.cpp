// The UI's protocol client and models against the mock engine on a real TCP socket.

#include "connection.hpp"
#include "recorder.hpp"
#include "selftest.hpp"
#include "strip_model.hpp"

#include "mock_engine.hpp"

#include "dc/control/client.hpp"

#include <QTest>

#include <memory>
#include <optional>
#include <thread>

using namespace dc;
using namespace std::chrono_literals;

namespace {

// Mock engine on its own thread, like dc-mock-server.
class Harness {
public:
    explicit Harness(std::uint16_t port = 0, std::uint64_t session = 77) {
        control::ControlServer::Options opt;
        opt.port = port;
        opt.serverName = "test-mock";
        opt.sessionId = session;
        server_ = std::make_unique<control::ControlServer>(io_, store_, opt);
        mock::MockEngine::Options mo;
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

private:
    asio::io_context io_;
    proto::ParamStore store_;
    std::unique_ptr<control::ControlServer> server_;
    std::unique_ptr<mock::MockEngine> engine_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::thread thread_;
};

ui::Connection::Options options(std::uint16_t port) {
    ui::Connection::Options o;
    o.port = port;
    o.reconnectMinMs = 50;
    o.reconnectMaxMs = 200;
    return o;
}

} // namespace

class UiTest : public QObject {
    Q_OBJECT

private slots:
    void selfTestPasses() {
        Harness h;
        ui::Connection c(options(h.port()));
        ui::StripModel tracks(c, ui::StripModel::Kind::Tracks);
        ui::StripModel master(c, ui::StripModel::Kind::Master);
        ui::Recorder recorder(c);
        c.start();
        QStringList log;
        const int failures = ui::runSelfTest(c, tracks, master, recorder, [&](const QString& l) { log << l; });
        QVERIFY2(failures == 0, qPrintable(log.join('\n')));
    }

    void otherClientsChangesShow() {
        Harness h;
        ui::Connection c(options(h.port()));
        ui::StripModel tracks(c, ui::StripModel::Kind::Tracks);
        c.start();
        QVERIFY(ui::waitUntil([&] { return c.ready(); }, 3000));
        QCOMPARE(tracks.count(), 4);
        QCOMPARE(tracks.data(tracks.index(0), ui::StripModel::NameRole).toString(), QStringLiteral("Mic"));
        QCOMPARE(tracks.data(tracks.index(0), ui::StripModel::InputTextRole).toString(),
                 QStringLiteral("Mock USB Microphone"));

        control::ControlClient::Options o;
        o.port = h.port();
        o.clientName = "other";
        control::ControlClient other(o);
        QVERIFY(other.connect(3s));
        QCOMPARE(other.set({{"/track/2/volume_db", -12.0}, {"/track/2/name", std::string("Voice")}}).get().code,
                 proto::ErrorCode::Ok);
        QVERIFY(ui::waitUntil([&] { return tracks.data(tracks.index(1), ui::StripModel::VolumeDbRole).toDouble() == -12.0; },
                              2000));
        QCOMPARE(tracks.data(tracks.index(1), ui::StripModel::NameRole).toString(), QStringLiteral("Voice"));

        // Track added and moved by someone else.
        const auto added = other.call("/mixer/track/add").get();
        QCOMPARE(added.code, proto::ErrorCode::Ok);
        QVERIFY(ui::waitUntil([&] { return tracks.count() == 5; }, 2000));
        QCOMPARE(other.call("/mixer/track/move", {{"track", std::int64_t{5}}, {"position", std::int64_t{1}}}).get().code,
                 proto::ErrorCode::Ok);
        QVERIFY(ui::waitUntil([&] { return tracks.trackIdAt(0) == 5; }, 2000));
        QCOMPARE(tracks.trackIdAt(1), 1);
        other.disconnect();
    }

    void reconnectsAfterEngineRestart() {
        std::uint16_t port = 0;
        auto h = std::make_unique<Harness>();
        port = h->port();
        ui::Connection c(options(port));
        ui::StripModel tracks(c, ui::StripModel::Kind::Tracks);
        c.start();
        QVERIFY(ui::waitUntil([&] { return c.ready(); }, 3000));
        tracks.addTrack(1);
        QVERIFY(ui::waitUntil([&] { return tracks.count() == 5; }, 2000));

        h.reset();
        QVERIFY(ui::waitUntil([&] { return !c.ready(); }, 3000));
        // Requests while disconnected fail instead of hanging.
        std::optional<proto::Result> r;
        c.call("/capture/record/start", {}, [&](const proto::Result& x) { r = x; });
        QVERIFY(ui::waitUntil([&] { return r.has_value(); }, 1000));
        QCOMPARE(r->code, proto::ErrorCode::Internal);

        h = std::make_unique<Harness>(port, 78);
        QVERIFY(ui::waitUntil([&] { return c.ready(); }, 5000));
        QCOMPARE(c.welcome().sessionId, std::uint64_t{78});
        QCOMPARE(tracks.count(), 4); // rebuilt from the new engine's snapshot
    }
};

QTEST_GUILESS_MAIN(UiTest)
#include "test_ui.moc"
