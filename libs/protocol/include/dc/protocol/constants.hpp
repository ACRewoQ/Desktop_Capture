#pragma once

#include <cstdint>

namespace dc::proto {

// Bump the major version on any incompatible change to docs/spec/protocol.md.
inline constexpr std::uint32_t kProtocolVersion = 1;

inline constexpr std::uint16_t kDefaultControlPort = 47800; // TCP
inline constexpr std::uint16_t kDefaultMeterPort = 47801;   // UDP, client side default

// Upper bound for one framed message. A full snapshot of 64 tracks is well below 1 MiB.
inline constexpr std::uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

inline constexpr const char* kMdnsServiceType = "_dcmix._tcp";

} // namespace dc::proto
