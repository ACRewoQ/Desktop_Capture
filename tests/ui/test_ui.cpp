// The UI's protocol client and models against the mock engine on a real TCP socket.

#include "connection.hpp"
#include "monitor_audio.hpp"
#include "param_hub.hpp"
#include "preview_stream.hpp"
#include "recorder.hpp"
#include "remote_check.hpp"
#include "route_choices.hpp"
#include "selftest.hpp"
#include "strip_model.hpp"

#include "mock_engine.hpp"

#include "dc/control/client.hpp"
#include "dc/lanmedia/audio_sender.hpp"
#include "dc/lanmedia/udp_transport.hpp"
#include "dc/lanmedia/video_link.hpp"

#include <QJSEngine>
#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <cmath>
#include <numbers>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

using namespace dc;
using namespace std::chrono_literals;

namespace {

// Mock engine on its own thread, like dc-mock-server.
class Harness {
public:
    // setup: register extra methods (what the mock engine does not have) before the server starts.
    explicit Harness(std::uint16_t port = 0, std::uint64_t session = 77,
                     const std::function<void(control::ControlServer&)>& setup = {}) {
        control::ControlServer::Options opt;
        opt.port = port;
        opt.serverName = "test-mock";
        opt.sessionId = session;
        server_ = std::make_unique<control::ControlServer>(io_, store_, opt);
        if (setup) setup(*server_);
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

    void remoteCheckPasses() {
        // G4 from a remote: edits, scene switch and a recording across a reconnect, seen by a second
        // connection; everything put back.
        Harness h;
        const auto o = options(h.port());
        ui::Connection c(o);
        ui::Recorder recorder(c);
        c.start();
        QStringList log;
        const int failures = ui::runRemoteCheck(c, recorder, o, [&](const QString& l) { log << l; });
        QVERIFY2(failures == 0, qPrintable(log.join('\n')));
        QVERIFY(log.size() >= 14);
    }

    void previewSubscribesOnlyWhileWatched() {
        // mixd's side of protocol.md 10: records the subscription, number 7.
        std::mutex m;
        int subscribes = 0, unsubscribes = 0;
        std::string stream;
        std::int64_t port = 0;
        Harness h(0, 77, [&](control::ControlServer& s) {
            s.registerMethod("/lan/subscribe", [&](control::CallContext& ctx) {
                std::lock_guard lock(m);
                ++subscribes;
                stream = std::get<std::string>(ctx.call.args.at("stream"));
                port = std::get<std::int64_t>(ctx.call.args.at("port"));
                proto::Result r;
                r.value = std::int64_t{7};
                return r;
            });
            s.registerMethod("/lan/unsubscribe", [&](control::CallContext& ctx) {
                std::lock_guard lock(m);
                if (std::get<std::int64_t>(ctx.call.args.at("id")) == 7) ++unsubscribes;
                return proto::Result{};
            });
        });
        ui::Connection c(options(h.port()));
        ui::PreviewStream preview(c);
        c.start();
        QVERIFY(ui::waitUntil([&] { return c.ready(); }, 3000));
        ui::waitUntil([] { return false; }, 100);
        QVERIFY(!preview.subscribed());  // nobody watching: capd does not encode
        QCOMPARE(subscribes, 0);
        QVERIFY(preview.statusText().isEmpty());

        std::atomic<int> frames{0};
        std::atomic<bool> firstIsKey{false};
        const int sink = preview.attach([&](const ui::PreviewStream::Frame& f) {
            if (frames++ == 0) firstIsKey = f.keyframe;
        });
        QVERIFY(ui::waitUntil([&] { return preview.subscribed(); }, 2000));
        QCOMPARE(preview.subscription(), 7);
        {
            std::lock_guard lock(m);
            QCOMPARE(subscribes, 1);
            QCOMPARE(stream, std::string("video/preview"));
            QCOMPARE(port, std::int64_t{preview.localPort()});
        }
        QVERIFY(preview.statusText().startsWith(QStringLiteral("映像を待って")));

        // capd's side: the stream to the subscriber's port, starting mid-GOP (the viewer asks for a keyframe).
        lan::VideoOutput::Options oo;
        oo.bindAddress = "127.0.0.1";
        oo.port = 0;
        lan::VideoOutput out(oo);
        out.setDestinations({lan::UdpTransport::Endpoint(asio::ip::make_address("127.0.0.1"), preview.localPort())});
        std::vector<std::uint8_t> au(5000, 0x42);
        for (int f = 0; f < 40; ++f) {
            out.send(au, f > 0 && out.keyframeWanted(), lan::nowSeconds());
            ui::waitUntil([] { return false; }, 5);
        }
        QVERIFY(ui::waitUntil([&] { return frames >= 20; }, 2000));
        QVERIFY(firstIsKey);
        QVERIFY(ui::waitUntil([&] { return preview.receiving(); }, 2000));
        QVERIFY2(preview.statusText().isEmpty(), qPrintable(preview.statusText()));
        QVERIFY(preview.infoText().contains(QStringLiteral("受信")));
        // The engine's clock (same machine: offset about 0) gives the delay from capture to arrival.
        QVERIFY(ui::waitUntil([&] { return preview.clockKnown(); }, 2000));
        QVERIFY(std::abs(preview.toLocalTime(100.0) - 100.0) < 0.005);
        for (int f = 0; f < 20; ++f) {
            out.send(au, false, lan::nowSeconds());
            ui::waitUntil([] { return false; }, 5);
        }
        QVERIFY(ui::waitUntil([&] { return preview.networkMs() >= 0; }, 2000));
        QVERIFY2(preview.networkMs() < 50, qPrintable(QString::number(preview.networkMs())));
        QVERIFY(preview.infoText().contains(QStringLiteral("届くまで")));

        // The viewer leaves: unsubscribed, and its sink is not called any more.
        preview.detach(sink);
        QVERIFY(!preview.subscribed());
        QVERIFY(ui::waitUntil([&] { std::lock_guard lock(m); return unsubscribes == 1; }, 2000));
        const int before = frames;
        out.send(au, true, lan::nowSeconds());
        ui::waitUntil([] { return false; }, 100);
        QCOMPARE(int(frames), before);
    }

    void nextSceneStreamSubscribesToVideoNext() {
        // The scene page's picture: the same viewer class on capd's video/next stream.
        std::mutex m;
        std::vector<std::string> streams;
        Harness h(0, 77, [&](control::ControlServer& s) {
            s.registerMethod("/lan/subscribe", [&](control::CallContext& ctx) {
                std::lock_guard lock(m);
                streams.push_back(std::get<std::string>(ctx.call.args.at("stream")));
                proto::Result r;
                r.value = std::int64_t{static_cast<std::int64_t>(streams.size())};
                return r;
            });
            s.registerMethod("/lan/unsubscribe", [&](control::CallContext&) { return proto::Result{}; });
        });
        ui::Connection c(options(h.port()));
        ui::PreviewStream next(c, "video/next", lan::kDefaultNextVideoPort);
        c.start();
        QVERIFY(ui::waitUntil([&] { return c.ready(); }, 3000));
        ui::waitUntil([] { return false; }, 100);
        QVERIFY(!next.subscribed());
        const int sink = next.attach([](const ui::PreviewStream::Frame&) {});
        QVERIFY(ui::waitUntil([&] { return next.subscribed(); }, 2000));
        {
            std::lock_guard lock(m);
            QCOMPARE(streams, std::vector<std::string>{"video/next"});
        }
        next.detach(sink);
        QVERIFY(!next.subscribed());
    }

    void monitorAudioPlaysTheChosenStream() {
        // mixd's side: subscription number 9 (and 10 for the second stream), stream and port recorded.
        std::mutex m;
        std::vector<std::string> streams;
        std::vector<std::int64_t> unsubscribed;
        std::int64_t port = 0;
        Harness h(0, 77, [&](control::ControlServer& s) {
            s.registerMethod("/lan/subscribe", [&](control::CallContext& ctx) {
                std::lock_guard lock(m);
                streams.push_back(std::get<std::string>(ctx.call.args.at("stream")));
                port = std::get<std::int64_t>(ctx.call.args.at("port"));
                proto::Result r;
                r.value = std::int64_t{8 + static_cast<std::int64_t>(streams.size())};
                return r;
            });
            s.registerMethod("/lan/unsubscribe", [&](control::CallContext& ctx) {
                std::lock_guard lock(m);
                unsubscribed.push_back(std::get<std::int64_t>(ctx.call.args.at("id")));
                return proto::Result{};
            });
        });
        ui::Connection c(options(h.port()));
        ui::MonitorAudio monitor(c, ui::MonitorAudio::Options{true});
        c.start();
        QVERIFY(ui::waitUntil([&] { return c.ready(); }, 3000));
        ui::waitUntil([] { return false; }, 100);
        QVERIFY(!monitor.subscribed());  // off: nothing asked for
        QVERIFY(monitor.statusText().isEmpty());

        monitor.setStream(2);
        QVERIFY(ui::waitUntil([&] { return monitor.subscribed(); }, 2000));
        QCOMPARE(monitor.subscription(), 9);
        {
            std::lock_guard lock(m);
            QCOMPARE(streams.size(), std::size_t{1});
            QCOMPARE(streams[0], std::string("audio/2"));
            QCOMPARE(port, std::int64_t{monitor.localPort()});
        }

        // mixd's sender: a -12 dBFS 1 kHz tone in Opus, 10 ms blocks on the real clock.
        std::atomic<bool> sending{true};
        std::atomic<std::uint16_t> to{monitor.localPort()};
        std::thread sender([&] {
            lan::UdpTransport::Options to_;
            to_.bindAddress = "127.0.0.1";
            to_.port = 0;
            lan::UdpTransport udp(to_);
            udp.start([](std::span<const std::uint8_t>, const lan::UdpTransport::Endpoint&, double) {});
            lan::AudioSender::Config sc;
            sc.encoder.codec = lan::AudioCodec::Opus;
            sc.encoder.frames = 480;
            lan::AudioSender tx(sc);
            std::vector<float> block(480 * 2);
            std::uint64_t pos = 0;
            auto next = std::chrono::steady_clock::now();
            while (sending) {
                for (std::size_t i = 0; i < 480; ++i, ++pos)
                    block[2 * i] = block[2 * i + 1] = 0.25f * static_cast<float>(std::sin(2 * std::numbers::pi * 1000.0 * double(pos) / 48000.0));
                tx.push(block.data(), 480, lan::nowSeconds());
                const lan::UdpTransport::Endpoint ep(asio::ip::make_address("127.0.0.1"), to.load());
                tx.pump([&](std::span<const std::uint8_t> d) { udp.send(d, ep); });
                next += 10ms;
                std::this_thread::sleep_until(next);
            }
            udp.stop();
        });
        QVERIFY(ui::waitUntil([&] { return monitor.playing(); }, 3000));
        QVERIFY2(monitor.statusText().isEmpty(), qPrintable(monitor.statusText()));
        QVERIFY(ui::waitUntil([&] { return monitor.level() > -14 && monitor.level() < -10; }, 2000));
        QVERIFY2(monitor.infoText().startsWith(QStringLiteral("Opus")), qPrintable(monitor.infoText()));
        monitor.setVolume(0.5);
        QVERIFY(ui::waitUntil([&] { return monitor.level() > -20 && monitor.level() < -16; }, 2000));

        // Another stream: the first subscription ends, a new one starts on a new port.
        monitor.setStream(3);
        QVERIFY(ui::waitUntil([&] { return monitor.subscription() == 10; }, 2000));
        to = monitor.localPort();
        QVERIFY(ui::waitUntil([&] { return monitor.playing(); }, 3000));

        // Off: unsubscribed, silent.
        monitor.setStream(0);
        QVERIFY(!monitor.subscribed());
        QCOMPARE(monitor.localPort(), std::uint16_t{0});
        QVERIFY(ui::waitUntil([&] { std::lock_guard lock(m); return unsubscribed.size() == 2; }, 2000));
        {
            std::lock_guard lock(m);
            QCOMPARE(unsubscribed[0], std::int64_t{9});
            QCOMPARE(unsubscribed[1], std::int64_t{10});
        }
        QCOMPARE(monitor.level(), -120.0);
        sending = false;
        sender.join();
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
