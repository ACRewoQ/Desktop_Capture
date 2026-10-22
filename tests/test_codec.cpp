#include "dc/protocol/codec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace dc::proto;

namespace {

Message roundTrip(const Message& m) {
    const auto bytes = encodePayload(m);
    auto r = decodePayload(bytes);
    REQUIRE(r.message.has_value());
    return *r.message;
}

} // namespace

TEST_CASE("every message type survives an encode/decode round trip", "[codec]") {
    const std::vector<Message> messages = {
        Hello{1, "ui", ClientRole::Remote, 47801, "123456", "tok"},
        Welcome{1, "mixd", "server-1", 0xFFFFFFFFFFull, "tok"},
        Snapshot{42, {{"/a", true}, {"/b", std::int64_t{-5}}, {"/c", 1.5}, {"/d", std::string("x")}, {"/e", {}}}},
        Delta{43, 7, "ui", {{"/track/1/volume_db", -6.0}}, {"/track/3"}},
        Set{9, {{"/track/1/mute", true}, {"/track/1/pan", -0.25}}},
        Call{10, "/mixer/track/add", {{"channels", std::int64_t{1}}, {"name", std::string("Mic 2")}}},
        Result{10, ErrorCode::Ok, "", "", std::int64_t{5}},
        Result{11, ErrorCode::OutOfRange, "too loud", "/track/1/volume_db", {}},
        Event{"capture.stopped", {{"file", std::string("a.mkv")}, {"duration_ms", std::int64_t{1234}}}},
        Ping{123},
        Pong{123},
        Error{ErrorCode::UnsupportedVersion, "nope"},
        Bye{"shutdown"},
    };
    for (const auto& m : messages) {
        INFO(typeName(typeOf(m)));
        CHECK(roundTrip(m) == m);
    }
}

TEST_CASE("large and negative integers keep their value", "[codec]") {
    Set s{1, {{"/x", std::int64_t{-9223372036854775807LL}}, {"/y", std::int64_t{9223372036854775807LL}}}};
    CHECK(roundTrip(s) == Message{s});
}

TEST_CASE("frame decoder reassembles frames split at any byte", "[codec]") {
    const Message a = Set{1, {{"/track/1/volume_db", -3.0}}};
    const Message b = Call{2, "/capture/record/start", {}};
    Bytes stream = encodeFrame(a);
    const Bytes fb = encodeFrame(b);
    stream.insert(stream.end(), fb.begin(), fb.end());

    for (std::size_t chunk = 1; chunk <= stream.size(); ++chunk) {
        FrameDecoder dec;
        std::vector<Message> got;
        for (std::size_t pos = 0; pos < stream.size(); pos += chunk) {
            const auto n = std::min(chunk, stream.size() - pos);
            dec.feed({stream.data() + pos, n});
            while (auto p = dec.next()) got.push_back(*decodePayload(*p).message);
        }
        REQUIRE(got.size() == 2);
        CHECK(got[0] == a);
        CHECK(got[1] == b);
        CHECK(dec.buffered() == 0);
    }
}

TEST_CASE("frame decoder rejects oversize and empty frames", "[codec]") {
    FrameDecoder dec(1024);
    const std::uint8_t header[4] = {0x01, 0x10, 0x00, 0x00}; // 4097 bytes
    dec.feed(header);
    CHECK_FALSE(dec.next().has_value());
    CHECK(dec.failed());

    FrameDecoder dec2;
    const std::uint8_t zero[4] = {0, 0, 0, 0};
    dec2.feed(zero);
    CHECK_FALSE(dec2.next().has_value());
    CHECK(dec2.failed());
}

TEST_CASE("malformed payloads produce an error instead of throwing", "[codec]") {
    const std::vector<Bytes> bad = {
        {},                         // empty
        {0xc1},                     // never-used msgpack byte
        {0x92, 0x63, 0x80},         // type 99
        {0x92, 0x05, 0x01},         // body is not a map
        {0x91, 0x05},               // missing body
        {0x92, 0x05, 0x81, 0xa2, 'i', 'd', 0xa1, 'x'}, // id is a string
    };
    for (const auto& b : bad) {
        auto r = decodePayload(b);
        CHECK_FALSE(r.message.has_value());
        CHECK_FALSE(r.error.empty());
    }
}

TEST_CASE("unknown fields are ignored for forward compatibility", "[codec]") {
    // [4, {"rev": 2, "future": true}]
    const Bytes payload = {0x92, 0x04, 0x82, 0xa3, 'r', 'e', 'v', 0x02, 0xa6, 'f', 'u', 't', 'u', 'r', 'e', 0xc3};
    auto r = decodePayload(payload);
    REQUIRE(r.message.has_value());
    CHECK(std::get<Delta>(*r.message).rev == 2);
}
