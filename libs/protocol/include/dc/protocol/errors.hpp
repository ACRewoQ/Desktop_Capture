#pragma once

#include <cstdint>

namespace dc::proto {

// Error codes carried in Result and Error messages. Values are part of the wire format.
enum class ErrorCode : std::uint16_t {
    Ok = 0,
    BadRequest = 1,         // malformed message or arguments
    NotFound = 2,           // path or method does not exist
    TypeMismatch = 3,       // value type does not match the parameter
    OutOfRange = 4,         // value outside min/max or not one of the enum choices
    ReadOnly = 5,           // parameter is server-managed
    UnsupportedVersion = 6, // protocol major version mismatch
    Unauthorized = 7,       // pairing / token required or rejected
    Conflict = 8,           // rejected by state, e.g. routing loop, already recording
    Internal = 9,
    UnknownMethod = 10,
    LimitReached = 11,      // e.g. 64 tracks already exist
};

const char* errorName(ErrorCode code);

} // namespace dc::proto
