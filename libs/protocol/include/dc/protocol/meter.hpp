#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dc::proto {

// UDP level meter packets, ~30 Hz, server -> client. Lost or late packets are simply dropped.
// Layout (little-endian) is documented in docs/spec/protocol.md "メーター（UDP）".
inline constexpr std::uint8_t kMeterMagic[4] = {'D', 'C', 'M', '1'};
inline constexpr std::size_t kMeterHeaderBytes = 20;
inline constexpr std::size_t kMaxMeterEntries = 128;
inline constexpr std::int16_t kMeterSilenceCdb = -14400; // centi-dB

struct MeterEntry {
    std::uint16_t meterId = 0;
    std::uint8_t channels = 2; // 1 or 2
    std::uint8_t flags = 0;    // bit0 = clipped since last packet, bit1 = input disconnected
    std::int16_t peakCdb[2] = {kMeterSilenceCdb, kMeterSilenceCdb};
    std::int16_t rmsCdb[2] = {kMeterSilenceCdb, kMeterSilenceCdb};
    bool operator==(const MeterEntry&) const = default;
};

struct MeterPacket {
    std::uint32_t seq = 0;
    std::uint64_t timeUs = 0; // engine clock (QPC converted to microseconds)
    std::vector<MeterEntry> entries;
    bool operator==(const MeterPacket&) const = default;
};

enum : std::uint8_t { kMeterFlagClip = 1u << 0, kMeterFlagDisconnected = 1u << 1 };

std::vector<std::uint8_t> encodeMeterPacket(const MeterPacket& p);
std::optional<MeterPacket> decodeMeterPacket(std::span<const std::uint8_t> data);

// Converts a linear amplitude (1.0 = 0 dBFS) to centi-dB, clamped to [-144 dB, +24 dB].
std::int16_t linearToCdb(float amplitude);

} // namespace dc::proto
