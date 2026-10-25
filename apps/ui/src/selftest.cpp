#include "selftest.hpp"

#include "connection.hpp"
#include "recorder.hpp"
#include "strip_model.hpp"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <cmath>
#include <optional>

namespace dc::ui {

bool waitUntil(const std::function<bool()>& pred, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() >= timeoutMs) return false;
        QEventLoop loop;
        QTimer::singleShot(5, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return true;
}

int runSelfTest(Connection& connection, StripModel& tracks, StripModel& master, Recorder& recorder,
                const std::function<void(const QString&)>& log, SelfTestOptions options) {
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        log((ok ? QStringLiteral("  OK   ") : QStringLiteral("  FAIL ")) + what);
        if (!ok) ++failures;
        return ok;
    };
    const auto& tree = connection.tree();
    auto num = [&](const std::string& path) { return tree.getAs<double>(path); };
    auto settled = [&](StripModel& m) { return !m.hasPendingEdits(); };

    if (!check(waitUntil([&] { return connection.ready(); }, options.connectTimeoutMs),
               QStringLiteral("connected to %1").arg(connection.address())))
        return failures;

    const auto trackCount = tree.getAs<std::int64_t>("/mixer/track_count").value_or(-1);
    check(tracks.count() == trackCount, QStringLiteral("strips match /mixer/track_count (%1)").arg(trackCount));
    check(master.count() == 1, QStringLiteral("master strip present"));

    // + button
    const int before = tracks.count();
    tracks.addTrack(2);
    if (!check(waitUntil([&] { return tracks.count() == before + 1; }, 2000), QStringLiteral("track added")))
        return failures;
    const int id = tracks.trackIdAt(tracks.count() - 1);
    const std::string base = "/track/" + std::to_string(id);
    auto row = [&] { return tracks.rowOfTrack(id); };
    check(tracks.data(tracks.index(row()), StripModel::ChannelsRole).toInt() == 2,
          QStringLiteral("new track %1 is stereo").arg(id));

    // Input picker: the first device the engine lists goes in with kind, source and channel together.
    const QVariantList inputs = tracks.inputChoices(row());
    QVariantMap device;
    for (const auto& v : inputs)
        if (v.toMap().value("kind") == QLatin1String("device")) {
            device = v.toMap();
            break;
        }
    check(!inputs.isEmpty() && inputs.first().toMap().value("kind") == QLatin1String("none"),
          QStringLiteral("input choices listed (%1)").arg(inputs.size()));
    if (!device.isEmpty()) {
        const std::string source = device.value("source").toString().toStdString();
        tracks.setInput(row(), QStringLiteral("device"), device.value("source").toString(), device.value("channel").toInt());
        check(waitUntil([&] { return tree.getAs<std::string>(base + "/input/kind") == std::string("device") &&
                                     tree.getAs<std::string>(base + "/input/source") == source && settled(tracks); },
                        2000),
              QStringLiteral("input set to %1").arg(device.value("label").toString()));
        const bool selected = waitUntil([&] {
            for (const auto& v : tracks.inputChoices(row())) {
                const auto m = v.toMap();
                if (m.value("selected").toBool()) return m.value("source").toString().toStdString() == source;
            }
            return false;
        }, 500);
        check(selected, QStringLiteral("picker marks the chosen input"));
    }
    const QVariantList outputs = master.outputChoices();
    int selectedOutputs = 0;
    for (const auto& v : outputs) selectedOutputs += v.toMap().value("selected").toBool() ? 1 : 0;
    check(outputs.size() >= 1 && selectedOutputs == 1,
          QStringLiteral("output choices listed (%1), current one marked").arg(outputs.size()));

    tracks.setVolumeDb(row(), -6.0);
    check(tracks.data(tracks.index(row()), StripModel::VolumeDbRole).toDouble() == -6.0,
          QStringLiteral("fader moves before the server answers"));
    check(waitUntil([&] { return num(base + "/volume_db") == -6.0 && settled(tracks); }, 2000),
          QStringLiteral("volume -6 dB reaches the server"));

    // A drag: many moves in a row are thinned out, and the last one wins.
    for (int i = 0; i <= 40; ++i) {
        tracks.setFader(row(), 0.2 + 0.015 * i);
        waitUntil([] { return false; }, 2);
    }
    const double lastDb = StripModel::faderToDb(0.2 + 0.015 * 40);
    check(waitUntil([&] { return num(base + "/volume_db") && std::abs(*num(base + "/volume_db") - lastDb) < 1e-9 &&
                                 settled(tracks); },
                    2000),
          QStringLiteral("fader drag ends on the last value (%1 dB)").arg(lastDb, 0, 'f', 2));

    tracks.toggleMute(row());
    check(waitUntil([&] { return tree.getAs<bool>(base + "/mute") == true && settled(tracks); }, 2000),
          QStringLiteral("mute on"));
    tracks.toggleMute(row());
    check(waitUntil([&] { return tree.getAs<bool>(base + "/mute") == false && settled(tracks); }, 2000),
          QStringLiteral("mute off"));
    tracks.toggleSolo(row());
    check(waitUntil([&] { return tree.getAs<bool>(base + "/solo") == true && settled(tracks); }, 2000),
          QStringLiteral("solo on"));

    tracks.setPan(row(), 0.5);
    tracks.toggleLowcut(row());
    tracks.setLowcutHz(row(), 120);
    tracks.toggleHighcut(row());
    tracks.setHighcutHz(row(), 12000);
    check(waitUntil([&] { return num(base + "/pan") == 0.5 && tree.getAs<bool>(base + "/lowcut/enabled") == true &&
                                 num(base + "/lowcut/freq_hz") == 120.0 &&
                                 tree.getAs<bool>(base + "/highcut/enabled") == true &&
                                 num(base + "/highcut/freq_hz") == 12000.0 && settled(tracks); },
                    2000),
          QStringLiteral("pan, low cut and high cut reach the server"));

    const int from = row();
    tracks.moveTrack(from, -1);
    check(waitUntil([&] { return row() == from - 1; }, 2000), QStringLiteral("track moved left"));

    tracks.removeTrack(row());
    check(waitUntil([&] { return tracks.count() == before && row() < 0 && !tree.contains(base + "/name"); }, 2000),
          QStringLiteral("track removed"));

    master.setVolumeDb(0, -3.0);
    check(waitUntil([&] { return num("/master/volume_db") == -3.0 && settled(master); }, 2000),
          QStringLiteral("master volume -3 dB"));
    master.setVolumeDb(0, 0.0);
    waitUntil([&] { return settled(master); }, 2000);

    // Recording
    recorder.start();
    check(waitUntil([&] { return recorder.recording() && !recorder.busy(); }, 5000), QStringLiteral("recording started"));
    // (Arguments are evaluated in any order: wait first, then build the message.)
    const bool running = waitUntil([&] { return tree.getAs<std::int64_t>("/capture/elapsed_ms").value_or(0) > 0; }, 2000);
    check(running, QStringLiteral("elapsed time runs (%1 ms)")
                       .arg(tree.getAs<std::int64_t>("/capture/elapsed_ms").value_or(0)));
    recorder.stop();
    const bool stopped = waitUntil([&] { return recorder.state() == QLatin1String("idle") && !recorder.busy(); }, 2000);
    check(stopped, QStringLiteral("recording stopped (%1)").arg(recorder.file()));

    // An error from the server reaches the toast.
    std::optional<QString> failure;
    auto conn = QObject::connect(&recorder, &Recorder::failed, [&](const QString& m) { failure = m; });
    recorder.stop();
    const bool reported = waitUntil([&] { return failure.has_value(); }, 2000);
    check(reported, QStringLiteral("stop while idle is reported: %1").arg(failure.value_or(QString())));
    QObject::disconnect(conn);

    if (options.expectMeters) {
        const bool meters = waitUntil([&] { return tracks.meterPackets() > 3 && master.meterPackets() > 3; }, 2000);
        check(meters, QStringLiteral("meters arrive (%1 packets)").arg(tracks.meterPackets()));
    }

    return failures;
}

} // namespace dc::ui
