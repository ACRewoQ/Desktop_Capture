#pragma once

// Hotkeys and MIDI bindings (docs/spec/protocol.md 5.9): the trigger strings, the key names with their
// Windows virtual-key codes, MIDI message decoding, the check that gives /control/binding/#/state and
// the mapping of MIDI values to parameters. Shared by the engine (which acts on bindings) and the UIs
// (which build and show them).

#include "dc/protocol/param_tree.hpp"
#include "dc/protocol/schema.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dc::proto::ctl {

// modifier bits of a key trigger, in the order they are written
enum Mod : std::uint8_t { ModCtrl = 1, ModAlt = 2, ModShift = 4, ModWin = 8 };

enum class MidiKind : std::uint8_t { Note, Cc, Pc, Pb };

struct Trigger {
    enum class Type : std::uint8_t { None, Key, Midi };
    Type type = Type::None;
    // Key
    std::uint8_t mods = 0;
    std::string key; // a name from keyNames()
    // Midi
    MidiKind midi = MidiKind::Note;
    int channel = 1; // 1..16
    int number = 0;  // 0..127, unused for pitch bend
    bool operator==(const Trigger&) const = default;
};

// "" -> None. Returns nullopt (and a reason) for anything malformed.
std::optional<Trigger> parseTrigger(std::string_view text, std::string* error = nullptr);
std::string formatTrigger(const Trigger& t);

// Key names in a stable order (letters, digits, function keys, keypad, navigation, punctuation, media).
const std::vector<std::string>& keyNames();
// Windows virtual-key code of a key name (0 = unknown) and back ("" = not a nameable key).
int virtualKeyOf(std::string_view name);
std::string keyNameOf(int virtualKey);

// One decoded MIDI channel message. value: velocity or CC value (0..127), program for pc, 0..16383 for pb.
struct MidiMessage {
    MidiKind kind = MidiKind::Note;
    int channel = 1;
    int number = 0;
    int value = 0;
    bool press = false;   // note on with velocity, CC >= 64, program change
    bool release = false; // note off (or velocity 0), CC < 64
};
// Short message bytes (status, data1, data2). Nothing for system messages and the rest.
std::optional<MidiMessage> decodeMidi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2);
Trigger triggerOf(const MidiMessage& m);
// The message's value as 0..1 (pb uses 14 bits, notes: press = 1, release = 0).
double normalizedValue(const MidiMessage& m);

// What /control/binding/N holds.
struct Binding {
    std::string trigger;
    std::string device;
    std::string action = "none";
    std::string target;
    std::string value;
    bool enabled = true;
};
Binding readBinding(const ParamTree& tree, int n);

// The binding's /control/binding/N/state and error from the current tree: "off", "invalid" (with a
// Japanese reason) or "ok". Platform states (conflict, unsupported) are the engine's business.
struct BindingCheck {
    std::string state;
    std::string error;
};
BindingCheck checkBinding(const ParamTree& tree, const Binding& b, const Schema& schema = schemaV1());

// Number range of a parameter for step and param: schema min/max, or the built-in effect's range
// (VST3 parameters 0..1). log = map MIDI values logarithmically.
struct Range {
    double min = 0;
    double max = 1;
    bool log = false;
    bool integer = false;
};
std::optional<Range> numberRange(const ParamTree& tree, std::string_view path, const Schema& schema = schemaV1());

// Reads a set value ("true", "3", "-6.5", "fade") as the parameter's type; nothing when it does not fit.
std::optional<Value> parseValue(const ParamTree& tree, std::string_view path, std::string_view text,
                                const Schema& schema = schemaV1());
// current + delta, clamped to the range.
std::optional<Value> stepValue(const ParamTree& tree, std::string_view path, double delta,
                               const Schema& schema = schemaV1());
// A MIDI value 0..1 as the parameter's value (protocol.md 5.9.4).
std::optional<Value> valueFromMidi(const ParamTree& tree, std::string_view path, double normalized,
                                   const Schema& schema = schemaV1());

// UI fader scale shared with the mixer strips: 0 = silence, otherwise -60..+12 dB linear.
inline constexpr double kFaderMinDb = -60.0;
double faderToDb(double position);
double dbToFader(double db);

// "/sampler/trigger" target "<pad>" or "<bank>/<pad>" -> bank (0 = active bank) and pad.
struct PadRef {
    int bank = 0;
    int pad = 0;
};
std::optional<PadRef> parsePad(std::string_view target);

} // namespace dc::proto::ctl
