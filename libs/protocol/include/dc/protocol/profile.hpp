#pragma once

// Which parameters a profile and a mixer snapshot keep (docs/spec/protocol.md 5.10). Shared by the
// engine (which saves and restores them) and the schema document (the 保存 column).

#include "dc/protocol/schema.hpp"

#include <string_view>

namespace dc::proto {

// Kept in a profile file: the writable settings of the mixer, effects, capture, recorder, sampler,
// video mixer and LAN streams, plus the read-only values that describe their structure (track order,
// effect types, scene and layer order, source kinds). Not: device lists, states, the plugin list,
// pairings, bindings (mixd-control.txt) and the PC-wide settings (/profile/autosave, /plugins/extra_dirs).
bool inProfile(const ParamSpec& spec);

// Kept in a mixer snapshot (a subset of the profile): fader, pan, mute, solo, Lo-Hi filters, send
// levels and effect bypass and parameters of the tracks and the master, and the cue volume.
// Not: names, inputs, routing targets, channels, recording, effect types.
bool inSnapshot(const ParamSpec& spec);

// A profile name is also its file name: 1 to 64 characters, no control characters, none of
// \ / : * ? " < > |, no leading or trailing space or dot, not a
// name Windows reserves (con, nul, com1...).
bool isValidProfileName(std::string_view name);

} // namespace dc::proto
