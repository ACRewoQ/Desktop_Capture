#pragma once

#include <QString>

#include <functional>

namespace dc::ui {

class Connection;
class Recorder;
class StripModel;

// G2 integration run (scripts/g2-integration.ps1): sets the first track's input through the picker
// logic, optionally starts a recording, then keeps moving the track faders, pans and filter
// frequencies (and toggles mutes) like a busy user, at the UI's 60 Hz send rate, while the screen
// draws. The master strip is left alone: it carries mixd's test pulse. Changes the engine's state.
struct ExerciseOptions {
    double minutes = 1;
    QString input;     // "" = leave the input, "first" = first input device, else a device id
    int inputChannel = 1;
    bool record = false;
    int connectTimeoutMs = 10000;
};

// Returns the number of failures; progress and a final "RESULT ..." line go to log.
int runExercise(Connection& connection, StripModel& tracks, Recorder& recorder,
                const std::function<void(const QString&)>& log, const ExerciseOptions& options);

} // namespace dc::ui
