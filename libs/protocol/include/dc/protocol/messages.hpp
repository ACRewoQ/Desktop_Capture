#pragma once

#include "dc/protocol/errors.hpp"
#include "dc/protocol/value.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace dc::proto {

// Wire identifiers. See docs/spec/protocol.md "メッセージ一覧".
enum class MsgType : std::uint8_t {
    Hello = 1,
    Welcome = 2,
    Snapshot = 3,
    Delta = 4,
    Set = 5,
    Call = 6,
    Result = 7,
    Event = 8,
    Ping = 9,
    Pong = 10,
    Error = 11,
    Bye = 12,
};

struct Change {
    std::string path;
    Value value;
    bool operator==(const Change&) const = default;
};

using Args = std::map<std::string, Value>;

// Peer = another engine (mixd) on the LAN that receives this engine's streams.
enum class ClientRole : std::uint8_t { Local = 0, Remote = 1, Service = 2, Peer = 3 };

// client -> server, first message on a connection
struct Hello {
    std::uint32_t proto = 0;
    std::string clientName;
    ClientRole role = ClientRole::Local;
    std::uint16_t meterPort = 0; // UDP port for meter packets, 0 = no meters
    std::string pairCode;        // remote / peer, first connection
    std::string token;           // remote / peer, after pairing
    std::string clientId;        // stable id of the client installation (a peer: its server_id)
    bool operator==(const Hello&) const = default;
};

// server -> client, answer to Hello
struct Welcome {
    std::uint32_t proto = 0;
    std::string serverName;
    std::string serverId;    // stable per installation
    std::uint64_t sessionId = 0; // changes every time the engine starts
    std::string token;       // issued after successful pairing
    bool operator==(const Welcome&) const = default;
};

// server -> client, full parameter tree
struct Snapshot {
    std::uint64_t rev = 0;
    std::vector<Change> params; // sorted by path
    bool operator==(const Snapshot&) const = default;
};

// server -> all clients, incremental change
struct Delta {
    std::uint64_t rev = 0;          // always previous rev + 1
    std::uint64_t originId = 0;     // request id of the Set/Call that caused it (0 = engine)
    std::string originClient;       // clientName of the originator ("" = engine)
    std::vector<Change> changes;    // added or modified paths
    std::vector<std::string> removed; // removed subtrees (prefixes)
    bool operator==(const Delta&) const = default;
};

// client -> server, change one or more parameters atomically
struct Set {
    std::uint64_t id = 0;
    std::vector<Change> changes;
    bool operator==(const Set&) const = default;
};

// client -> server, invoke a command
struct Call {
    std::uint64_t id = 0;
    std::string method; // e.g. "/mixer/track/add"
    Args args;
    bool operator==(const Call&) const = default;
};

// server -> client, answer to Set or Call
struct Result {
    std::uint64_t id = 0;
    ErrorCode code = ErrorCode::Ok;
    std::string message;  // human readable detail on error
    std::string path;     // offending path on error, if any
    Value value;          // return value of a Call
    bool operator==(const Result&) const = default;
};

// server -> clients, notification that is not a parameter (device lost, recording file closed...)
struct Event {
    std::string name;
    Args data;
    bool operator==(const Event&) const = default;
};

struct Ping {
    std::uint64_t t = 0;
    bool operator==(const Ping&) const = default;
};

struct Pong {
    std::uint64_t t = 0;
    bool operator==(const Pong&) const = default;
};

// fatal error; the sender closes the connection afterwards
struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;
    bool operator==(const Error&) const = default;
};

struct Bye {
    std::string reason;
    bool operator==(const Bye&) const = default;
};

using Message = std::variant<Hello, Welcome, Snapshot, Delta, Set, Call, Result, Event, Ping, Pong, Error, Bye>;

MsgType typeOf(const Message& m);
const char* typeName(MsgType t);

} // namespace dc::proto
