#include "remote_check.hpp"

#include "recorder.hpp"
#include "selftest.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace dc::ui {

using namespace dc::proto;

namespace {

bool sameValue(const ParamTree& a, const ParamTree& b, const std::string& path) {
    const Value* x = a.get(path);
    const Value* y = b.get(path);
    return x && y && *x == *y;
}

// Paths whose values the engine changes by itself all the time (meters, timers, buffer levels): the
// two mirrors may be one delta apart on them.
bool isLive(const std::string& path) {
    for (const char* s : {"/elapsed_ms", "_delay_ms", "/lan/", "/remote/", "/stats/", "/level", "/devices/", "/state"})
        if (path.find(s) != std::string::npos) return true;
    return false;
}

} // namespace

int runRemoteCheck(Connection& connection, Recorder& recorder, const Connection::Options& otherOptions,
                   const std::function<void(const QString&)>& log) {
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        log((ok ? QStringLiteral("  OK   ") : QStringLiteral("  FAIL ")) + what);
        if (!ok) ++failures;
        return ok;
    };
    const ParamTree& mine = connection.tree();
    if (!check(waitUntil([&] { return connection.ready(); }, 10000), QStringLiteral("connected to %1").arg(connection.address())))
        return failures;

    Connection::Options oo = otherOptions;
    oo.clientName = QStringLiteral("g4-other");
    oo.meters = false;
    Connection other(oo);
    other.start();
    // It is a remote too: with 4 remotes on the engine already it gets limit_reached.
    if (!check(waitUntil([&] { return other.ready(); }, 10000),
               QStringLiteral("second connection (another screen) up (%1)").arg(other.statusText())))
        return failures;
    const ParamTree& theirs = other.tree();

    // Puts a value and waits until the other screen shows the same; remembers the old one.
    std::vector<Change> undo;
    auto setAndSee = [&](const std::string& path, const Value& v, const QString& what) {
        if (const Value* old = mine.get(path)) undo.push_back({path, *old});
        bool ok = false;
        connection.set({{path, v}}, [&](const Result& r) { ok = r.code == ErrorCode::Ok; });
        const bool seen = waitUntil([&] { return ok && sameValue(mine, theirs, path) && mine.get(path) && *mine.get(path) == v; }, 3000);
        check(seen, what);
    };

    // Mixer
    std::string strip = "/master";
    for (int t = 1; t <= 64; ++t)
        if (mine.contains("/track/" + std::to_string(t) + "/volume_db")) {
            strip = "/track/" + std::to_string(t);
            break;
        }
    setAndSee(strip + "/volume_db", Value(-12.5), QStringLiteral("fader on %1 seen by the other screen").arg(QString::fromStdString(strip)));
    setAndSee(strip + "/pan", Value(-0.25), QStringLiteral("pan seen by the other screen"));
    setAndSee(strip + "/mute", Value(!mine.getAs<bool>(strip + "/mute").value_or(false)), QStringLiteral("mute seen by the other screen"));

    // Scene switch on mix 1: a second scene if there is only one, then preview it and TAKE.
    std::optional<std::int64_t> addedScene;
    std::vector<std::int64_t> scenes;
    for (int n = 1; n <= 64; ++n)
        if (mine.contains("/video/scene/" + std::to_string(n) + "/name")) scenes.push_back(n);
    if (scenes.size() < 2) {
        std::int64_t created = 0;
        connection.call("/video/scene/add", {{"name", std::string("G4 check")}}, [&](const Result& r) {
            if (const auto* n = std::get_if<std::int64_t>(&r.value)) created = *n;
        });
        waitUntil([&] { return created > 0; }, 3000);
        if (created > 0) {
            addedScene = created;
            scenes.push_back(created);
        }
    }
    const auto program = mine.getAs<std::int64_t>("/video/mix/1/program_scene");
    if (check(program && scenes.size() >= 2, QStringLiteral("mix 1 with two scenes"))) {
        const std::int64_t next = scenes[0] == *program ? scenes[1] : scenes[0];
        const auto previewBefore = mine.getAs<std::int64_t>("/video/mix/1/preview_scene");
        setAndSee("/video/mix/1/preview_scene", Value(next), QStringLiteral("preview scene set"));
        undo.pop_back();  // TAKE swaps it anyway; put back below
        bool took = false;
        connection.call("/video/mix/1/take", {}, [&](const Result& r) { took = r.code == ErrorCode::Ok; });
        check(waitUntil([&] { return took && mine.getAs<std::int64_t>("/video/mix/1/program_scene") == next &&
                                     theirs.getAs<std::int64_t>("/video/mix/1/program_scene") == next; }, 3000),
              QStringLiteral("TAKE: scene %1 on air, seen by the other screen").arg(next));
        // back to the scene that was on air
        connection.call("/video/mix/1/take");
        waitUntil([&] { return mine.getAs<std::int64_t>("/video/mix/1/program_scene") == *program; }, 3000);
        if (previewBefore) undo.push_back({"/video/mix/1/preview_scene", Value(*previewBefore)});
    }

    // A recording setting.
    const auto bitrate = mine.getAs<std::int64_t>("/capture/video/bitrate_kbps").value_or(20000);
    setAndSee("/capture/video/bitrate_kbps", Value(std::int64_t{bitrate == 12345 ? 12346 : 12345}),
              QStringLiteral("recording setting seen by the other screen"));
    // put it back before recording, so the recording uses the user's settings
    connection.set({{"/capture/video/bitrate_kbps", Value(bitrate)}});
    undo.pop_back();

    // Recording across a reconnect of this remote.
    if (check(!recorder.recording(), QStringLiteral("not recording before the test"))) {
        recorder.start();
        const bool started = waitUntil([&] { return recorder.recording(); }, 10000);
        if (check(started, QStringLiteral("recording started from the remote (%1)").arg(recorder.file()))) {
            const std::string file = mine.getAs<std::string>("/capture/file").value_or("");
            waitUntil([] { return false; }, 3000);
            connection.stop();  // the remote goes away (Wi-Fi drop, app closed)
            waitUntil([] { return false; }, 3000);
            const bool stillThere = theirs.getAs<std::string>("/capture/state") == std::string("recording") &&
                                    theirs.getAs<std::string>("/capture/file") == file;
            check(stillThere, QStringLiteral("recording goes on while the remote is away (other screen: %1)")
                                  .arg(QString::fromStdString(theirs.getAs<std::string>("/capture/state").value_or("?"))));
            connection.start();
            check(waitUntil([&] { return connection.ready(); }, 10000), QStringLiteral("remote reconnected"));
            const auto elapsed = mine.getAs<std::int64_t>("/capture/elapsed_ms").value_or(0);
            check(recorder.recording() && mine.getAs<std::string>("/capture/file") == file && elapsed >= 5500,
                  QStringLiteral("same recording after the reconnect (%1 s, same file)").arg(double(elapsed) / 1000.0, 0, 'f', 1));
            waitUntil([] { return false; }, 1000);
            recorder.stop();
            check(waitUntil([&] { return !recorder.recording() && !recorder.busy() &&
                                         theirs.getAs<std::string>("/capture/state") != std::string("recording"); }, 10000),
                  QStringLiteral("recording stopped from the remote (%1)").arg(recorder.error().isEmpty() ? recorder.file() : recorder.error()));
        }
    }

    // Put everything back.
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) connection.set({*it});
    if (addedScene) connection.call("/video/scene/remove", {{"scene", *addedScene}});
    waitUntil([] { return false; }, 1000);

    // Both mirrors hold the same tree (apart from values the engine updates all the time).
    int differences = 0;
    QString first;
    const bool same = waitUntil([&] {
        differences = 0;
        first.clear();
        for (const auto& [path, v] : mine.values()) {
            if (isLive(path)) continue;
            const Value* o = theirs.get(path);
            if (!o || !(*o == v)) {
                if (first.isEmpty()) first = QString::fromStdString(path);
                ++differences;
            }
        }
        for (const auto& [path, v] : theirs.values())
            if (!isLive(path) && !mine.get(path)) ++differences;
        return differences == 0;
    }, 3000);
    check(same, QStringLiteral("this screen and the other show the same tree (%1 values%2)")
                    .arg(mine.values().size())
                    .arg(differences ? QStringLiteral(", %1 differ, e.g. %2").arg(differences).arg(first) : QString()));
    other.stop();
    return failures;
}

} // namespace dc::ui
