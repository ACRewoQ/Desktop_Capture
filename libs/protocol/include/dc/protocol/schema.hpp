#pragma once

#include "dc/protocol/errors.hpp"
#include "dc/protocol/value.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dc::proto {

// Limits shared by every process. See docs/spec/protocol.md "上限値".
inline constexpr int kMaxTracks = 64;
inline constexpr int kInitialTracks = 4;
inline constexpr int kMaxTrackOutputs = 8;
inline constexpr int kMaxTrackSends = 8;
inline constexpr int kMaxFxPerTrack = 16;
inline constexpr int kMaxVideoMixes = 4;
inline constexpr int kMaxVideoSources = 64;
inline constexpr int kMaxVideoScenes = 32;
inline constexpr int kMaxLayersPerScene = 32;
inline constexpr int kMaxCaptureAudioStreams = 8;
inline constexpr int kMaxLanStreams = 8;        // /lan/stream/1..8, output target lan/<n>
inline constexpr int kMaxLanSubscriptions = 32; // /lan/sub/#
inline constexpr int kMaxLanPeers = 16;         // /lan/peers/#
inline constexpr int kMaxPairedClients = 16;    // /remote/paired/#
inline constexpr int kSamplerBanks = 8;
inline constexpr int kSamplerPads = 16; // per bank
inline constexpr int kMaxBindings = 128;   // /control/binding/# (hotkeys and MIDI)
inline constexpr int kMaxMidiDevices = 16; // /control/midi/device/#
inline constexpr int kMaxSnapshots = 64;   // /snapshot/# (mixer snapshots of the current profile)
inline constexpr int kMaxProfiles = 64;    // /profile/list/#
inline constexpr double kMinVolumeDb = -144.0; // treated as -inf (silence)
inline constexpr double kMaxVolumeDb = 12.0;

// LocalOnly: written by the engine PC's own clients only. A remote controller (role 1) reads it but its
// Set is refused with unauthorized (the sampler library folder, protocol.md 5.4.3).
enum class Access : std::uint8_t { ReadWrite, ReadOnly, LocalOnly };

// Describes one parameter pattern. In a pattern, "#" matches an index segment (1, 2, ...)
// and "*" matches any single name segment (used for plugin parameter names).
struct ParamSpec {
    std::string pattern;
    ValueType type = ValueType::Nil;
    Access access = Access::ReadWrite;
    std::optional<double> min;              // Int / Float
    std::optional<double> max;              // Int / Float
    std::vector<std::string> choices;       // String enums; empty = free text
    std::vector<std::int64_t> intChoices;   // Int enums; empty = any value in range
    Value defaultValue;
    std::string description;                // Japanese, used by the spec table
};

struct MethodSpec {
    std::string pattern;
    std::string args;        // documentation only, e.g. "channels:int?, name:string?"
    std::string returns;     // documentation only
    std::string description; // Japanese
    std::string since;       // phase that implements it, e.g. "P0", "P3"
};

struct ValidationError {
    ErrorCode code = ErrorCode::Ok;
    std::string message;
    explicit operator bool() const { return code != ErrorCode::Ok; }
};

class Schema {
public:
    void add(ParamSpec spec);
    void addMethod(MethodSpec spec);

    const ParamSpec* find(std::string_view path) const;
    const MethodSpec* findMethod(std::string_view method) const;

    // Checks type, range and choices. Ints are accepted for Float parameters.
    // Access is checked only when fromClient is true.
    ValidationError validate(const ParamSpec& spec, const Value& v, bool fromClient) const;

    // Converts an accepted value to its canonical type (Int -> Float for Float params).
    static Value normalize(const ParamSpec& spec, Value v);

    const std::vector<ParamSpec>& params() const { return params_; }
    const std::vector<MethodSpec>& methods() const { return methods_; }

    // Replaces index segments by "#": "/track/12/volume_db" -> "/track/#/volume_db".
    static std::string patternOf(std::string_view path);

private:
    std::vector<ParamSpec> params_;
    std::vector<MethodSpec> methods_;
    std::unordered_map<std::string, std::size_t> index_;
    std::unordered_map<std::string, std::size_t> methodIndex_;
};

// The schema of protocol version 1. Engine and mock server both validate against it.
const Schema& schemaV1();

// Valid path syntax: starts with "/", segments of [a-z0-9_.-] (index segments are digits without
// a leading zero), no empty segments, no trailing "/".
bool isValidPath(std::string_view path);

} // namespace dc::proto
