#pragma once

// Which parameters a profile and a mixer snapshot keep (docs/spec/protocol.md 5.10). Shared by the
// engine (which saves and restores them) and the schema document (the 保存 column).

#include "dc/protocol/messages.hpp"
#include "dc/protocol/param_tree.hpp"
#include "dc/protocol/schema.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// --- the profile file ---

struct SavedDevice {
    std::string id;
    std::string name;
    bool input = false;
    bool operator==(const SavedDevice&) const = default;
};

struct SavedSnapshot {
    int number = 0;
    std::string name;
    std::int64_t modified = 0; // UNIX seconds
    std::vector<Change> values; // snapshotValues(): inSnapshot values plus the effect types they belong to
    bool operator==(const SavedSnapshot&) const = default;
};

struct ProfileData {
    std::vector<Change> values;                  // profileValues()
    std::vector<SavedDevice> devices;            // names of the devices the settings refer to
    std::map<std::string, std::string> fxState;  // "/track/3/fx/2" -> the plugin's own state (binary)
    std::vector<SavedSnapshot> snapshots;
    bool operator==(const ProfileData&) const = default;
};

// UTF-8 text, one item per line (tab separated, \\ \t \n \r escaped), versioned, ends with "end".
std::string writeProfile(const ProfileData& profile);
// nullopt (and why) when the text is not a whole profile file: no header, a newer format, cut short.
// Lines that cannot be read are skipped and counted.
std::optional<ProfileData> readProfile(std::string_view text, std::string* error = nullptr, int* skippedLines = nullptr);

// The values of a tree that a profile keeps (inProfile), in path order.
std::vector<Change> profileValues(const ParamTree& tree, const Schema& schema = schemaV1());
// The values a snapshot keeps (inSnapshot), plus /…/fx/N/type of every slot so a recall can tell
// whether a slot still holds the same effect.
std::vector<Change> snapshotValues(const ParamTree& tree, const Schema& schema = schemaV1());
// Device ids the settings refer to: device inputs, device/ and vmic/ targets, the cue, the clock device.
std::vector<std::string> deviceRefs(const ParamTree& tree);

// How to turn the current tree into a profile's state.
struct FxRestore {
    std::string fileSlot; // the slot's base in the file ("/track/3/fx/2"), the key of ProfileData::fxState
    std::string type;
    std::vector<std::pair<std::string, Value>> values; // "/bypass", "/param/<key>"
};
struct RestorePlan {
    std::vector<std::string> removed; // subtrees to clear first
    std::vector<Change> changes;      // everything but the effect slots
    std::map<int, std::vector<FxRestore>> fx; // strip (0 = master) -> slots in order
    int skipped = 0;                  // values that did not fit and kept their defaults
};
// values: a profile's values (ProfileData::values). Numbers of tracks, sources, scenes and mixes are
// kept, positions renumbered 1..n in their saved order. Every value is checked like a client's Set
// (schema, routing, video references, effect ranges); what fails is skipped.
RestorePlan planRestore(const ParamTree& current, const std::vector<Change>& values, const Schema& schema = schemaV1());

// The Set that recalls a snapshot: values of tracks that still exist and of effect slots that still hold
// the same type, and only paths the tree has.
std::vector<Change> planRecall(const ParamTree& tree, const std::vector<Change>& snapshot);

// Standard base64 (for plugin states in the file).
std::string base64Encode(std::string_view data);
std::optional<std::string> base64Decode(std::string_view text);

} // namespace dc::proto
