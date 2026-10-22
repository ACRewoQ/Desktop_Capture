#include "dc/protocol/errors.hpp"
#include "dc/protocol/messages.hpp"
#include "dc/protocol/value.hpp"

#include <cstdio>

namespace dc::proto {

const char* typeName(ValueType t) {
    switch (t) {
    case ValueType::Nil: return "nil";
    case ValueType::Bool: return "bool";
    case ValueType::Int: return "int";
    case ValueType::Float: return "float";
    case ValueType::String: return "string";
    }
    return "?";
}

std::string toDisplayString(const Value& v) {
    switch (typeOf(v)) {
    case ValueType::Nil: return "nil";
    case ValueType::Bool: return std::get<bool>(v) ? "true" : "false";
    case ValueType::Int: return std::to_string(std::get<std::int64_t>(v));
    case ValueType::Float: {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", std::get<double>(v));
        return buf;
    }
    case ValueType::String: return "\"" + std::get<std::string>(v) + "\"";
    }
    return "?";
}

const char* errorName(ErrorCode code) {
    switch (code) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::BadRequest: return "bad_request";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::TypeMismatch: return "type_mismatch";
    case ErrorCode::OutOfRange: return "out_of_range";
    case ErrorCode::ReadOnly: return "read_only";
    case ErrorCode::UnsupportedVersion: return "unsupported_version";
    case ErrorCode::Unauthorized: return "unauthorized";
    case ErrorCode::Conflict: return "conflict";
    case ErrorCode::Internal: return "internal";
    case ErrorCode::UnknownMethod: return "unknown_method";
    case ErrorCode::LimitReached: return "limit_reached";
    }
    return "unknown";
}

MsgType typeOf(const Message& m) {
    return static_cast<MsgType>(m.index() + 1);
}

const char* typeName(MsgType t) {
    switch (t) {
    case MsgType::Hello: return "Hello";
    case MsgType::Welcome: return "Welcome";
    case MsgType::Snapshot: return "Snapshot";
    case MsgType::Delta: return "Delta";
    case MsgType::Set: return "Set";
    case MsgType::Call: return "Call";
    case MsgType::Result: return "Result";
    case MsgType::Event: return "Event";
    case MsgType::Ping: return "Ping";
    case MsgType::Pong: return "Pong";
    case MsgType::Error: return "Error";
    case MsgType::Bye: return "Bye";
    }
    return "?";
}

} // namespace dc::proto
