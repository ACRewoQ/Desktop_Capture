#include "exercise.hpp"

#include "connection.hpp"
#include "recorder.hpp"
#include "selftest.hpp"
#include "strip_model.hpp"

#include <QElapsedTimer>
#include <QVariantMap>

#include <cmath>
#include <optional>

namespace dc::ui {

namespace {
constexpr double kPi = 3.14159265358979323846;
} // namespace

int runExercise(Connection& connection, StripModel& tracks, Recorder& recorder,
                const std::function<void(const QString&)>& log, const ExerciseOptions& options) {
    int failures = 0;
    auto fail = [&](const QString& what) {
        log(QStringLiteral("  FAIL ") + what);
        ++failures;
    };
    const auto& tree = connection.tree();

    if (!waitUntil([&] { return connection.ready(); }, options.connectTimeoutMs)) {
        fail(QStringLiteral("not connected to %1").arg(connection.address()));
        return failures;
    }
    if (tracks.count() == 0) {
        fail(QStringLiteral("no tracks"));
        return failures;
    }
    log(QStringLiteral("  OK   connected to %1, %2 tracks").arg(connection.address()).arg(tracks.count()));

    int errors = 0;
    QString lastError;
    auto conn = QObject::connect(&tracks, &StripModel::failed, [&](const QString& m) {
        ++errors;
        lastError = m;
    });
    auto recConn = QObject::connect(&recorder, &Recorder::failed, [&](const QString& m) {
        ++errors;
        lastError = QStringLiteral("録画: ") + m;
    });

    // Input of the first strip, chosen from the same list the picker shows.
    const int id = tracks.trackIdAt(0);
    const std::string base = "/track/" + std::to_string(id);
    if (!options.input.isEmpty()) {
        std::optional<QVariantMap> choice;
        for (const auto& v : tracks.inputChoices(0)) {
            const auto m = v.toMap();
            if (m.value("kind") != QLatin1String("device")) continue;
            if (options.input == QLatin1String("first") ||
                (m.value("source").toString() == options.input && m.value("channel").toInt() == options.inputChannel)) {
                choice = m;
                break;
            }
        }
        if (!choice) {
            fail(QStringLiteral("input %1 is not in the list").arg(options.input));
        } else {
            tracks.setInput(0, QStringLiteral("device"), choice->value("source").toString(), choice->value("channel").toInt());
            const bool ok = waitUntil([&] { return tree.getAs<std::string>(base + "/input/state") == std::string("ok"); }, 10000);
            const QString state = QString::fromStdString(tree.getAs<std::string>(base + "/input/state").value_or("?"));
            if (ok) log(QStringLiteral("  OK   track %1 input: %2 (state %3)").arg(id).arg(choice->value("label").toString(), state));
            else fail(QStringLiteral("track %1 input %2 did not open (state %3)").arg(id).arg(choice->value("label").toString(), state));
        }
    }
    const QString inputName = tracks.data(tracks.index(0), StripModel::InputTextRole).toString();

    if (options.record) {
        recorder.start();
        if (waitUntil([&] { return recorder.recording() && !recorder.busy(); }, 10000))
            log(QStringLiteral("  OK   recording (%1)").arg(recorder.summary()));
        else
            fail(QStringLiteral("recording did not start (state %1, %2)").arg(recorder.state(), recorder.error()));
    }

    // Busy user: every track's fader, pan and filter frequencies follow slow sine waves with
    // different phases; mutes and filters toggle now and then. One step per 16 ms.
    QElapsedTimer t;
    t.start();
    const qint64 total = static_cast<qint64>(options.minutes * 60000);
    qint64 nextLog = 60000;
    long long steps = 0;
    while (t.elapsed() < total) {
        const double s = static_cast<double>(t.elapsed()) / 1000.0;
        for (int row = 0; row < tracks.count(); ++row) {
            const double ph = row * 0.7;
            tracks.setFader(row, 0.55 + 0.25 * std::sin(2 * kPi * 0.3 * s + ph));
            tracks.setPan(row, 0.8 * std::sin(2 * kPi * 0.17 * s + ph));
            tracks.setLowcutHz(row, 200 + 150 * std::sin(2 * kPi * 0.11 * s + ph));
            tracks.setHighcutHz(row, 10000 + 6000 * std::sin(2 * kPi * 0.07 * s + ph));
        }
        if (steps % 125 == 0) { // every 2 s
            const int row = static_cast<int>((steps / 125) % tracks.count());
            tracks.toggleMute(row);
            tracks.toggleLowcut(row);
            tracks.toggleHighcut(row);
        }
        ++steps;
        if (options.record && !recorder.recording() && !recorder.busy()) {
            fail(QStringLiteral("recording stopped by itself at %1 s (state %2, %3)")
                     .arg(s, 0, 'f', 1)
                     .arg(recorder.state(), recorder.error()));
            break;
        }
        if (t.elapsed() >= nextLog) {
            nextLog += 60000;
            log(QStringLiteral("  ...  %1 min, %2 steps, xruns %3, recording %4")
                    .arg(t.elapsed() / 60000)
                    .arg(steps)
                    .arg(tree.getAs<std::int64_t>("/system/xruns").value_or(-1))
                    .arg(recorder.elapsedText()));
        }
        waitUntil([] { return false; }, 16);
    }
    waitUntil([&] { return !tracks.hasPendingEdits(); }, 2000);

    QString file;
    if (options.record && recorder.recording()) {
        recorder.stop();
        if (waitUntil([&] { return recorder.state() == QLatin1String("idle") && !recorder.busy(); }, 15000)) {
            file = recorder.file();
            log(QStringLiteral("  OK   recording stopped: %1").arg(file));
        } else {
            fail(QStringLiteral("recording did not stop (state %1)").arg(recorder.state()));
        }
    }
    QObject::disconnect(conn);
    QObject::disconnect(recConn);
    if (errors) fail(QStringLiteral("%1 requests refused, last: %2").arg(errors).arg(lastError));

    log(QStringLiteral("RESULT steps=%1 seconds=%2 errors=%3 input=\"%4\" file=\"%5\" failures=%6")
            .arg(steps)
            .arg(static_cast<double>(t.elapsed()) / 1000.0, 0, 'f', 1)
            .arg(errors)
            .arg(inputName, file)
            .arg(failures));
    return failures;
}

} // namespace dc::ui
