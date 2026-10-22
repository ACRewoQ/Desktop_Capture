#include "dc/protocol/codec.hpp"
#include "dc/protocol/constants.hpp"

#include <msgpack.hpp>

#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace dc::proto {

namespace {

using Packer = msgpack::packer<msgpack::sbuffer>;

// ---------- encoding ----------

// msgpack-c packs integral doubles as integers; the protocol keeps Float as float64 on the wire so
// the receiver sees the same type that was sent.
void packFloat64(Packer& pk, double d) {
    std::uint64_t bits;
    std::memcpy(&bits, &d, sizeof bits);
    char raw[9];
    raw[0] = static_cast<char>(0xcb);
    for (int i = 0; i < 8; ++i) raw[1 + i] = static_cast<char>((bits >> (56 - 8 * i)) & 0xFF);
    pk.pack_bin_body(raw, sizeof raw); // appends raw bytes without a header
}

void packValue(Packer& pk, const Value& v) {
    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                pk.pack_nil();
            } else if constexpr (std::is_same_v<T, bool>) {
                if (x) pk.pack_true(); else pk.pack_false();
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                pk.pack_int64(x);
            } else if constexpr (std::is_same_v<T, double>) {
                packFloat64(pk, x);
            } else {
                pk.pack_str(static_cast<std::uint32_t>(x.size()));
                pk.pack_str_body(x.data(), static_cast<std::uint32_t>(x.size()));
            }
        },
        v);
}

void packStr(Packer& pk, const std::string& s) {
    pk.pack_str(static_cast<std::uint32_t>(s.size()));
    pk.pack_str_body(s.data(), static_cast<std::uint32_t>(s.size()));
}

void packKey(Packer& pk, const char* key) {
    const auto n = static_cast<std::uint32_t>(std::strlen(key));
    pk.pack_str(n);
    pk.pack_str_body(key, n);
}

void packChanges(Packer& pk, const std::vector<Change>& changes) {
    pk.pack_map(static_cast<std::uint32_t>(changes.size()));
    for (const auto& c : changes) {
        packStr(pk, c.path);
        packValue(pk, c.value);
    }
}

void packArgs(Packer& pk, const Args& args) {
    pk.pack_map(static_cast<std::uint32_t>(args.size()));
    for (const auto& [k, v] : args) {
        packStr(pk, k);
        packValue(pk, v);
    }
}

struct BodyPacker {
    Packer& pk;

    void operator()(const Hello& m) {
        pk.pack_map(6);
        packKey(pk, "proto"); pk.pack_uint32(m.proto);
        packKey(pk, "client"); packStr(pk, m.clientName);
        packKey(pk, "role"); pk.pack_uint8(static_cast<std::uint8_t>(m.role));
        packKey(pk, "meter_port"); pk.pack_uint16(m.meterPort);
        packKey(pk, "pair_code"); packStr(pk, m.pairCode);
        packKey(pk, "token"); packStr(pk, m.token);
    }
    void operator()(const Welcome& m) {
        pk.pack_map(5);
        packKey(pk, "proto"); pk.pack_uint32(m.proto);
        packKey(pk, "server"); packStr(pk, m.serverName);
        packKey(pk, "server_id"); packStr(pk, m.serverId);
        packKey(pk, "session"); pk.pack_uint64(m.sessionId);
        packKey(pk, "token"); packStr(pk, m.token);
    }
    void operator()(const Snapshot& m) {
        pk.pack_map(2);
        packKey(pk, "rev"); pk.pack_uint64(m.rev);
        packKey(pk, "params"); packChanges(pk, m.params);
    }
    void operator()(const Delta& m) {
        pk.pack_map(5);
        packKey(pk, "rev"); pk.pack_uint64(m.rev);
        packKey(pk, "origin"); pk.pack_uint64(m.originId);
        packKey(pk, "origin_client"); packStr(pk, m.originClient);
        packKey(pk, "changes"); packChanges(pk, m.changes);
        packKey(pk, "removed");
        pk.pack_array(static_cast<std::uint32_t>(m.removed.size()));
        for (const auto& r : m.removed) packStr(pk, r);
    }
    void operator()(const Set& m) {
        pk.pack_map(2);
        packKey(pk, "id"); pk.pack_uint64(m.id);
        packKey(pk, "changes"); packChanges(pk, m.changes);
    }
    void operator()(const Call& m) {
        pk.pack_map(3);
        packKey(pk, "id"); pk.pack_uint64(m.id);
        packKey(pk, "method"); packStr(pk, m.method);
        packKey(pk, "args"); packArgs(pk, m.args);
    }
    void operator()(const Result& m) {
        pk.pack_map(5);
        packKey(pk, "id"); pk.pack_uint64(m.id);
        packKey(pk, "code"); pk.pack_uint16(static_cast<std::uint16_t>(m.code));
        packKey(pk, "message"); packStr(pk, m.message);
        packKey(pk, "path"); packStr(pk, m.path);
        packKey(pk, "value"); packValue(pk, m.value);
    }
    void operator()(const Event& m) {
        pk.pack_map(2);
        packKey(pk, "name"); packStr(pk, m.name);
        packKey(pk, "data"); packArgs(pk, m.data);
    }
    void operator()(const Ping& m) {
        pk.pack_map(1);
        packKey(pk, "t"); pk.pack_uint64(m.t);
    }
    void operator()(const Pong& m) {
        pk.pack_map(1);
        packKey(pk, "t"); pk.pack_uint64(m.t);
    }
    void operator()(const Error& m) {
        pk.pack_map(2);
        packKey(pk, "code"); pk.pack_uint16(static_cast<std::uint16_t>(m.code));
        packKey(pk, "message"); packStr(pk, m.message);
    }
    void operator()(const Bye& m) {
        pk.pack_map(1);
        packKey(pk, "reason"); packStr(pk, m.reason);
    }
};

// ---------- decoding ----------

struct DecodeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Value toValue(const msgpack::object& o) {
    switch (o.type) {
    case msgpack::type::NIL: return std::monostate{};
    case msgpack::type::BOOLEAN: return o.via.boolean;
    case msgpack::type::POSITIVE_INTEGER:
        if (o.via.u64 > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw DecodeError("integer out of int64 range");
        return static_cast<std::int64_t>(o.via.u64);
    case msgpack::type::NEGATIVE_INTEGER: return o.via.i64;
    case msgpack::type::FLOAT32:
    case msgpack::type::FLOAT64: return o.via.f64;
    case msgpack::type::STR: return std::string(o.via.str.ptr, o.via.str.size);
    default: throw DecodeError("unsupported value type");
    }
}

std::string toStr(const msgpack::object& o) {
    if (o.type != msgpack::type::STR) throw DecodeError("expected string");
    return std::string(o.via.str.ptr, o.via.str.size);
}

std::uint64_t toU64(const msgpack::object& o) {
    if (o.type != msgpack::type::POSITIVE_INTEGER) throw DecodeError("expected unsigned integer");
    return o.via.u64;
}

template <typename T>
T toUnsigned(const msgpack::object& o) {
    const auto v = toU64(o);
    if (v > std::numeric_limits<T>::max()) throw DecodeError("unsigned integer out of range");
    return static_cast<T>(v);
}

std::vector<Change> toChanges(const msgpack::object& o) {
    if (o.type != msgpack::type::MAP) throw DecodeError("expected map of changes");
    std::vector<Change> out;
    out.reserve(o.via.map.size);
    for (std::uint32_t i = 0; i < o.via.map.size; ++i) {
        const auto& kv = o.via.map.ptr[i];
        out.push_back({toStr(kv.key), toValue(kv.val)});
    }
    return out;
}

Args toArgs(const msgpack::object& o) {
    if (o.type != msgpack::type::MAP) throw DecodeError("expected map of arguments");
    Args out;
    for (std::uint32_t i = 0; i < o.via.map.size; ++i) {
        const auto& kv = o.via.map.ptr[i];
        out[toStr(kv.key)] = toValue(kv.val);
    }
    return out;
}

std::vector<std::string> toStrings(const msgpack::object& o) {
    if (o.type != msgpack::type::ARRAY) throw DecodeError("expected array of strings");
    std::vector<std::string> out;
    out.reserve(o.via.array.size);
    for (std::uint32_t i = 0; i < o.via.array.size; ++i) out.push_back(toStr(o.via.array.ptr[i]));
    return out;
}

// Calls fn(key, value) for every entry. Unknown keys are ignored by the callers (forward compatible).
template <typename Fn>
void forEachField(const msgpack::object& body, Fn&& fn) {
    if (body.type != msgpack::type::MAP) throw DecodeError("message body must be a map");
    for (std::uint32_t i = 0; i < body.via.map.size; ++i) {
        const auto& kv = body.via.map.ptr[i];
        fn(toStr(kv.key), kv.val);
    }
}

Message decodeBody(MsgType type, const msgpack::object& body) {
    switch (type) {
    case MsgType::Hello: {
        Hello m;
        forEachField(body, [&](const std::string& k, const msgpack::object& v) {
            if (k == "proto") m.proto = toUnsigned<std::uint32_t>(v);
            else if (k == "client") m.clientName = toStr(v);
            else if (k == "role") {
                const auto r = toUnsigned<std::uint8_t>(v);
                if (r > static_cast<std::uint8_t>(ClientRole::Service)) throw DecodeError("unknown role");
                m.role = static_cast<ClientRole>(r);
            }
            else if (k == "meter_port") m.meterPort = toUnsigned<std::uint16_t>(v);
            else if (k == "pair_code") m.pairCode = toStr(v);
            else if (k == "token") m.token = toStr(v);
        });
        return m;
    }
    case MsgType::Welcome: {
        Welcome m;
        forEachField(body, [&](const std::string& k, const msgpack::object& v) {
            if (k == "proto") m.proto = toUnsigned<std::uint32_t>(v);
            else if (k == "server") m.serverName = toStr(v);
            else if (k == "server_id") m.serverId = toStr(v);
            else if (k == "session") m.sessionId = toU64(v);
            else if (k == "token") m.token = toStr(v);
        });
        return m;
    }
    case MsgType::Snapshot: {
        Snapshot m;
        forEachField(body, [&](const std::string& k, const msgpack::object& v) {
            if (k == "rev") m.rev = toU64(v);
            else if (k == "params") m.params = toChanges(v);
        });
        return m;
    }
    case MsgType::Delta: {
        Delta m;
        forEachField(body, [&](const std::string& k, const msgpack::object& v) {
            if (k == "rev") m.rev = toU64(v);
            else if (k == "origin") m.originId = toU64(v);
            else if (k == "origin_client") m.originClient = toStr(v);
            else if (k == "changes") m.changes = toChanges(v);
            else if (k == "removed") m.removed = toStrings(v);
        });
        return m;
    }
    case MsgType::Set: {
        Set m;
        forEachField(body, [&](const std::string& k, const msgpack::object& v) {
            if (k == "id") m.id = toU64(v);
            else if (k == "changes") m.changes = toChanges(v);
        });
        return m;
    }
    case MsgType::Call: {
        Call m;
        forEachField(body, [&](const std::string& k, const msgpack::object& v) {
            if (k == "id") m.id = toU64(v);
            else if (k == "method") m.method = toStr(v);
            else if (k == "args") m.args = toArgs(v);
        });
        return m;
    }
    case MsgType::Result: {
        Result m;
        forEachField(body, [&](const std::string& k, const msgpack::object& v) {
            if (k == "id") m.id = toU64(v);
            else if (k == "code") m.code = static_cast<ErrorCode>(toUnsigned<std::uint16_t>(v));
            else if (k == "message") m.message = toStr(v);
            else if (k == "path") m.path = toStr(v);
            else if (k == "value") m.value = toValue(v);
        });
        return m;
    }
    case MsgType::Event: {
        Event m;
        forEachField(body, [&](const std::string& k, const msgpack::object& v) {
            if (k == "name") m.name = toStr(v);
            else if (k == "data") m.data = toArgs(v);
        });
        return m;
    }
    case MsgType::Ping: {
        Ping m;
        forEachField(body, [&](const std::string& k, const msgpack::object& v) {
            if (k == "t") m.t = toU64(v);
        });
        return m;
    }
    case MsgType::Pong: {
        Pong m;
        forEachField(body, [&](const std::string& k, const msgpack::object& v) {
            if (k == "t") m.t = toU64(v);
        });
        return m;
    }
    case MsgType::Error: {
        Error m;
        forEachField(body, [&](const std::string& k, const msgpack::object& v) {
            if (k == "code") m.code = static_cast<ErrorCode>(toUnsigned<std::uint16_t>(v));
            else if (k == "message") m.message = toStr(v);
        });
        return m;
    }
    case MsgType::Bye: {
        Bye m;
        forEachField(body, [&](const std::string& k, const msgpack::object& v) {
            if (k == "reason") m.reason = toStr(v);
        });
        return m;
    }
    }
    throw DecodeError("unknown message type");
}

} // namespace

Bytes encodePayload(const Message& msg) {
    msgpack::sbuffer buf;
    Packer pk(buf);
    pk.pack_array(2);
    pk.pack_uint8(static_cast<std::uint8_t>(typeOf(msg)));
    std::visit(BodyPacker{pk}, msg);
    const auto* p = reinterpret_cast<const std::uint8_t*>(buf.data());
    return Bytes(p, p + buf.size());
}

DecodeResult decodePayload(std::span<const std::uint8_t> payload) {
    try {
        msgpack::object_handle oh =
            msgpack::unpack(reinterpret_cast<const char*>(payload.data()), payload.size());
        const msgpack::object& root = oh.get();
        if (root.type != msgpack::type::ARRAY || root.via.array.size < 2)
            return {std::nullopt, "message must be an array [type, body]"};
        const auto rawType = toU64(root.via.array.ptr[0]);
        if (rawType < static_cast<std::uint64_t>(MsgType::Hello) ||
            rawType > static_cast<std::uint64_t>(MsgType::Bye))
            return {std::nullopt, "unknown message type " + std::to_string(rawType)};
        return {decodeBody(static_cast<MsgType>(rawType), root.via.array.ptr[1]), {}};
    } catch (const std::exception& e) {
        return {std::nullopt, e.what()};
    }
}

Bytes encodeFrame(const Message& msg) {
    Bytes payload = encodePayload(msg);
    const auto n = static_cast<std::uint32_t>(payload.size());
    Bytes out;
    out.reserve(payload.size() + 4);
    out.push_back(static_cast<std::uint8_t>(n & 0xFF));
    out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((n >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((n >> 24) & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

FrameDecoder::FrameDecoder(std::uint32_t maxFrameBytes) : maxFrameBytes_(maxFrameBytes) {}
FrameDecoder::FrameDecoder() : FrameDecoder(kMaxFrameBytes) {}

void FrameDecoder::feed(std::span<const std::uint8_t> data) {
    if (failed_) return;
    // Compact occasionally so the buffer does not grow without bound.
    if (readPos_ > 0 && readPos_ * 2 > buffer_.size()) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(readPos_));
        readPos_ = 0;
    }
    buffer_.insert(buffer_.end(), data.begin(), data.end());
}

std::optional<Bytes> FrameDecoder::next() {
    if (failed_ || buffered() < 4) return std::nullopt;
    const std::uint8_t* p = buffer_.data() + readPos_;
    const std::uint32_t n = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                            (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    if (n == 0 || n > maxFrameBytes_) {
        failed_ = true;
        return std::nullopt;
    }
    if (buffered() < 4 + static_cast<std::size_t>(n)) return std::nullopt;
    Bytes payload(p + 4, p + 4 + n);
    readPos_ += 4 + n;
    return payload;
}

} // namespace dc::proto
