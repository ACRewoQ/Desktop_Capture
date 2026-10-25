// The UI's protocol client and models against the mock engine on a real TCP socket.

#include "connection.hpp"
#include "param_hub.hpp"
#include "recorder.hpp"
#include "route_choices.hpp"
#include "selftest.hpp"
#include "strip_model.hpp"

#include "mock_engine.hpp"

#include "dc/control/client.hpp"

#include <QJSEngine>
#include <QSignalSpy>
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

    // Param / ParamList / calls for the pages that are not strips.
    void paramHubFollowsAndEdits() {
        Harness h;
        ui::Connection c(options(h.port()));
        ui::ParamHub hub(c);
        ui::ParamValue mode;
        mode.setPath(QStringLiteral("/system/audio_mode"));
        ui::ParamValue block;
        block.setPath(QStringLiteral("/system/block_size"));
        ui::ParamList list;
        list.setPrefix(QStringLiteral("/track"));
        list.setOrderBy(QStringLiteral("position"));
        // Schema facts are there before the engine answers.
        QCOMPARE(block.type(), QStringLiteral("int"));
        QCOMPARE(block.choices().size(), 5);
        QCOMPARE(mode.value().toString(), QStringLiteral("shared")); // default until the snapshot
        QVERIFY(!mode.exists());
        QSignalSpy modeChanged(&mode, &ui::ParamValue::valueChanged);

        c.start();
        QVERIFY(ui::waitUntil([&] { return c.ready(); }, 3000));
        QVERIFY(mode.exists());
        QVERIFY(modeChanged.count() >= 1); // the snapshot
        QCOMPARE(list.count(), 4);
        QCOMPARE(hub.indices(QStringLiteral("/devices/input")).size(), 2);

        // Edit: shown at once, confirmed by the engine, the int arrives as an int.
        block.set(128.0);
        QCOMPARE(block.value().toInt(), 128);
        QVERIFY(block.pending());
        QVERIFY(ui::waitUntil([&] { return !block.pending(); }, 2000));
        QCOMPARE(c.tree().getAs<std::int64_t>("/system/block_size").value_or(0), std::int64_t{128});

        // A rejected value goes back and is reported.
        QSignalSpy failed(&hub, &ui::ParamHub::failed);
        block.set(100);
        QVERIFY(ui::waitUntil([&] { return failed.count() == 1 && !block.pending(); }, 2000));
        QCOMPARE(block.value().toInt(), 128);

        // Another client's change reaches the Param.
        control::ControlClient::Options o;
        o.port = h.port();
        o.clientName = "other";
        control::ControlClient other(o);
        QVERIFY(other.connect(3s));
        QCOMPARE(other.set({{"/system/audio_mode", std::string("exclusive")}}).get().code, proto::ErrorCode::Ok);
        QVERIFY(ui::waitUntil([&] { return mode.value().toString() == QLatin1String("exclusive"); }, 2000));

        // Lists follow added, removed and reordered entries.
        QCOMPARE(other.call("/mixer/track/add").get().code, proto::ErrorCode::Ok);
        QVERIFY(ui::waitUntil([&] { return list.count() == 5; }, 2000));
        QCOMPARE(other.call("/mixer/track/move", {{"track", std::int64_t{5}}, {"position", std::int64_t{1}}}).get().code,
                 proto::ErrorCode::Ok);
        QVERIFY(ui::waitUntil([&] { return list.numAt(0) == 5; }, 2000));
        QCOMPARE(other.call("/mixer/track/remove", {{"track", std::int64_t{2}}}).get().code, proto::ErrorCode::Ok);
        QVERIFY(ui::waitUntil([&] { return list.count() == 4 && list.rowOf(2) < 0; }, 2000));

        // A Param below a removed subtree notices.
        ui::ParamValue name;
        name.setPath(QStringLiteral("/track/3/name"));
        QVERIFY(name.exists());
        QCOMPARE(other.call("/mixer/track/remove", {{"track", std::int64_t{3}}}).get().code, proto::ErrorCode::Ok);
        QVERIFY(ui::waitUntil([&] { return !name.exists(); }, 2000));

        // setMany: one Set; call with a JS callback (method arguments become ints).
        hub.setMany({{QStringLiteral("/track/1/name"), QStringLiteral("A")}, {QStringLiteral("/track/1/pan"), 0.5}});
        QVERIFY(ui::waitUntil([&] { return c.tree().getAs<std::string>("/track/1/name") == std::string("A"); }, 2000));
        QCOMPARE(c.tree().getAs<double>("/track/1/pan").value_or(0), 0.5);
        QJSEngine js;
        js.globalObject().setProperty(QStringLiteral("result"), QJSValue());
        QJSValue cb = js.evaluate(QStringLiteral("(function(ok, v) { result = [ok, v] })"));
        hub.call(QStringLiteral("/mixer/track/add"), {{QStringLiteral("channels"), 1.0}}, cb);
        QVERIFY(ui::waitUntil([&] { return js.globalObject().property("result").isArray(); }, 2000));
        QJSValue result = js.globalObject().property("result");
        QVERIFY(result.property(0).toBool());
        const int added = result.property(1).toInt();
        QVERIFY(added > 0);
        QCOMPARE(c.tree().getAs<std::int64_t>("/track/" + std::to_string(added) + "/channels").value_or(0), std::int64_t{1});
        other.disconnect();
    }

    void routeAndInputChoices() {
        Harness h;
        ui::Connection c(options(h.port()));
        ui::StripModel tracks(c, ui::StripModel::Kind::Tracks);
        ui::RouteChoices routes(c);
        c.start();
        QVERIFY(ui::waitUntil([&] { return c.ready(); }, 3000));
        auto values = [](const QVariantList& list) {
            QStringList v;
            for (const auto& e : list)
                if (e.toMap().contains("value")) v << e.toMap().value("value").toString();
            return v;
        };
        const QStringList out = values(routes.targetChoices("out", 1));
        QVERIFY(out.contains("master"));
        QVERIFY(out.contains("track/2"));
        QVERIFY(!out.contains("track/1")); // not itself
        QVERIFY(out.contains("device/out-spk"));
        QVERIFY(out.contains("lan/8"));
        QVERIFY(!values(routes.targetChoices("send", 1)).contains("master"));
        const QStringList cue = values(routes.targetChoices("cue"));
        QVERIFY(cue.contains("device/out-spk"));
        QVERIFY(!cue.contains("lan/1"));
        QCOMPARE(routes.targetText("master"), QStringLiteral("マスター"));
        QCOMPARE(routes.targetText("device/out-spk"), QStringLiteral("Mock Speakers"));
        QCOMPARE(routes.targetText("track/1"), QStringLiteral("Mic"));
        QCOMPARE(routes.targetText("lan/2"), QStringLiteral("LAN 2"));

        // Inputs: devices, applications and the sampler; the current one is marked.
        const QVariantList inputs = tracks.inputChoices(1); // "Game" plays game.exe
        int selected = 0;
        bool sampler = false;
        for (const auto& e : inputs) {
            const auto m = e.toMap();
            if (m.value("selected").toBool()) {
                ++selected;
                QCOMPARE(m.value("kind").toString(), QStringLiteral("process"));
                QCOMPARE(m.value("source").toString(), QStringLiteral("game.exe"));
            }
            sampler |= m.value("kind").toString() == QLatin1String("sampler") && m.value("source").toString() == "3";
        }
        QCOMPARE(selected, 1);
        QVERIFY(sampler);
        tracks.setInput(1, "sampler", "3", 1);
        QVERIFY(ui::waitUntil([&] { return tracks.inputTextAt(1) == QStringLiteral("サンプラー B3"); }, 2000));
    }

    void effectChain() {
        Harness h;
        ui::Connection c(options(h.port()));
        ui::StripModel tracks(c, ui::StripModel::Kind::Tracks);
        ui::ParamHub hub(c);
        ui::ParamList fxSlots;
        fxSlots.setPrefix(QStringLiteral("/track/1/fx"));
        c.start();
        QVERIFY(ui::waitUntil([&] { return c.ready(); }, 3000));
        QCOMPARE(hub.builtinEffects().size(), 6);
        hub.call(QStringLiteral("/track/1/fx/add"), {{QStringLiteral("type"), QStringLiteral("comp")}});
        hub.call(QStringLiteral("/track/1/fx/add"), {{QStringLiteral("type"), QStringLiteral("eq4")}});
        QVERIFY(ui::waitUntil([&] { return fxSlots.count() == 2; }, 2000));
        QCOMPARE(tracks.data(tracks.index(0), ui::StripModel::FxCountRole).toInt(), 2);
        ui::ParamValue firstName;
        firstName.setPath(QStringLiteral("/track/1/fx/1/name"));
        QCOMPARE(firstName.value().toString(), QStringLiteral("コンプレッサー"));
        // Slots are renumbered on move: the Param at slot 1 now shows the EQ.
        hub.call(QStringLiteral("/track/1/fx/move"), {{QStringLiteral("from"), 2}, {QStringLiteral("to"), 1}});
        QVERIFY(ui::waitUntil([&] { return firstName.value().toString() == QStringLiteral("4バンドEQ"); }, 2000));
        // A parameter edit within the built-in range.
        ui::ParamValue gain;
        gain.setPath(QStringLiteral("/track/1/fx/1/param/low_gain_db"));
        gain.set(6.5);
        QVERIFY(ui::waitUntil([&] { return !gain.pending(); }, 2000));
        QCOMPARE(c.tree().getAs<double>("/track/1/fx/1/param/low_gain_db").value_or(0), 6.5);
        hub.call(QStringLiteral("/track/1/fx/remove"), {{QStringLiteral("index"), 1}});
        QVERIFY(ui::waitUntil([&] { return fxSlots.count() == 1; }, 2000));
        QCOMPARE(tracks.data(tracks.index(0), ui::StripModel::FxCountRole).toInt(), 1);
    }

    void videoMixer() {
        Harness h;
        ui::Connection c(options(h.port()));
        ui::ParamHub hub(c);
        ui::ParamList layers;
        layers.setPrefix(QStringLiteral("/video/scene/1/layer"));
        layers.setOrderBy(QStringLiteral("position"));
        c.start();
        QVERIFY(ui::waitUntil([&] { return c.ready(); }, 3000));
        QCOMPARE(hub.indices(QStringLiteral("/video/source")), QList<int>({1}));
        QCOMPARE(layers.count(), 1);

        // A second layer goes to the front: the list is ordered by position, not by number.
        hub.call(QStringLiteral("/video/source/add"), {{QStringLiteral("kind"), QStringLiteral("color")}});
        QVERIFY(ui::waitUntil([&] { return hub.indices(QStringLiteral("/video/source")).size() == 2; }, 2000));
        hub.call(QStringLiteral("/video/scene/1/layer/add"), {{QStringLiteral("source"), 2}});
        QVERIFY(ui::waitUntil([&] { return layers.count() == 2; }, 2000));
        QCOMPARE(layers.numAt(0), 2);
        QCOMPARE(layers.numAt(1), 1);
        hub.call(QStringLiteral("/video/scene/1/layer/move"), {{QStringLiteral("layer"), 1}, {QStringLiteral("position"), 1}});
        QVERIFY(ui::waitUntil([&] { return layers.numAt(0) == 1; }, 2000));

        // A box preset is one Set of four floats (the JS numbers 0 and 1 become floats, not ints).
        const QString b = QStringLiteral("/video/scene/1/layer/2");
        hub.setMany({{b + "/x", 0.7}, {b + "/y", 0.7}, {b + "/width", 0.28}, {b + "/height", 1}});
        ui::ParamValue height;
        height.setPath(b + "/height");
        QVERIFY(ui::waitUntil([&] { return !height.pending(); }, 2000));
        QCOMPARE(c.tree().getAs<double>("/video/scene/1/layer/2/x").value_or(0), 0.7);
        QCOMPARE(c.tree().getAs<double>("/video/scene/1/layer/2/height").value_or(0), 1.0);

        // TAKE swaps preview and program.
        hub.call(QStringLiteral("/video/scene/add"), {}, QJSValue());
        QVERIFY(ui::waitUntil([&] { return hub.indices(QStringLiteral("/video/scene")).size() == 2; }, 2000));
        hub.set(QStringLiteral("/video/mix/1/preview_scene"), 2);
        QVERIFY(ui::waitUntil([&] { return c.tree().getAs<std::int64_t>("/video/mix/1/preview_scene").value_or(0) == 2; }, 2000));
        hub.call(QStringLiteral("/video/mix/1/take"));
        QVERIFY(ui::waitUntil([&] { return c.tree().getAs<std::int64_t>("/video/mix/1/program_scene").value_or(0) == 2; }, 2000));
        QCOMPARE(c.tree().getAs<std::int64_t>("/video/mix/1/preview_scene").value_or(0), 1);

        // Removing the source removes its layer.
        hub.call(QStringLiteral("/video/source/remove"), {{QStringLiteral("source"), 2}});
        QVERIFY(ui::waitUntil([&] { return layers.count() == 1; }, 2000));
    }

    void samplerAndRecorder() {
        Harness h;
        ui::Connection c(options(h.port()));
        ui::ParamHub hub(c);
        c.start();
        QVERIFY(ui::waitUntil([&] { return c.ready(); }, 3000));

        // A pad press as the sampler page sends it; the pad's playing flag follows.
        ui::ParamValue playing;
        playing.setPath(QStringLiteral("/sampler/bank/1/pad/1/playing"));
        QVERIFY(ui::waitUntil([&] { return hub.value(QStringLiteral("/sampler/bank/1/pad/1/state")).toString() == QStringLiteral("ready"); }, 2000));
        hub.call(QStringLiteral("/sampler/trigger"), {{QStringLiteral("pad"), 1}, {QStringLiteral("bank"), 1}});
        QVERIFY(ui::waitUntil([&] { return playing.value().toBool(); }, 2000));
        hub.call(QStringLiteral("/sampler/stop"));
        QVERIFY(ui::waitUntil([&] { return !playing.value().toBool(); }, 2000));

        // Per-track recording: arm one track, start and stop from the capture page's button.
        hub.set(QStringLiteral("/track/1/record/enabled"), true);
        ui::ParamValue state;
        state.setPath(QStringLiteral("/recorder/state"));
        QVERIFY(ui::waitUntil([&] { return c.tree().getAs<bool>("/track/1/record/enabled").value_or(false); }, 2000));
        hub.call(QStringLiteral("/recorder/start"));
        QVERIFY(ui::waitUntil([&] { return state.value().toString() == QStringLiteral("recording"); }, 2000));
        hub.call(QStringLiteral("/recorder/stop"));
        QVERIFY(ui::waitUntil([&] { return state.value().toString() == QStringLiteral("idle"); }, 2000));

        // A recording audio stream takes a track.
        hub.set(QStringLiteral("/capture/audio/4/source"), QStringLiteral("track/2"));
        QVERIFY(ui::waitUntil([&] { return c.tree().getAs<std::string>("/capture/audio/4/source").value_or("") == "track/2"; }, 2000));
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
