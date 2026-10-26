#pragma once

#include "connection.hpp"

#include <QString>

#include <functional>

namespace dc::ui {

class Recorder;

// G4 from the remote's side (dc-ui --remote --g4): with this connection and a second one opened with
// the same options (the way a second screen, e.g. the PC's own UI, sees the engine):
//   - mixer edits (fader, pan, mute), a scene switch (preview scene + TAKE) and a recording setting
//     reach the other connection with the same values;
//   - a recording started from here keeps running while this connection drops and comes back, and
//     stops from here; the trees of both connections are equal at the end;
//   - every value changed is put back.
// Needs at least one track and one video mix. Returns the number of failed checks.
int runRemoteCheck(Connection& connection, Recorder& recorder, const Connection::Options& otherOptions,
                   const std::function<void(const QString&)>& log);

} // namespace dc::ui
