#pragma once

#include <QString>

#include <functional>

namespace dc::ui {

class Connection;
class Recorder;
class StripModel;

// Scripted end-to-end check of the UI logic against a running engine or dc-mock-server: connect,
// add / edit / move / remove a track, master fader, record start / stop, meters. Used by
// `dc-ui --selftest` (scripts/test.ps1) and by the Qt test. Changes the server's state.
struct SelfTestOptions {
    int connectTimeoutMs = 5000;
    bool expectMeters = true;
};

// Returns the number of failed checks; every check is reported through log ("OK ..." / "FAIL ...").
int runSelfTest(Connection& connection, StripModel& tracks, StripModel& master, Recorder& recorder,
                const std::function<void(const QString&)>& log, SelfTestOptions options = {});

// Runs the event loop until pred() is true or the timeout expires. Returns pred().
bool waitUntil(const std::function<bool()>& pred, int timeoutMs);

} // namespace dc::ui
