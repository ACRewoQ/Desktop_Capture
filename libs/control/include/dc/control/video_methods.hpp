#pragma once

// Registers the video mixer methods (dc/protocol/video.hpp) on a control server, so the engine and
// the mock server share one implementation.

#include "dc/control/server.hpp"

namespace dc::control {

// /video/source/add, /video/scene/#/layer/move, /video/mix/#/take, the capd reports, ...
void registerVideoMethods(ControlServer& server);

// Call from ControlServer::Options::onDelta: republishes /video/mix/#/active when a delta touched
// the video tree or an output's mix number. Network thread.
void followVideoDelta(ControlServer& server, const proto::Delta& delta);

} // namespace dc::control
