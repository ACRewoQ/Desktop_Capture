#include "dc/protocol/meter.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace dc::proto;

TEST_CASE("meter packets round trip", "[meter]") {
    MeterPacket p;
    p.seq = 77;
    p.timeUs = 123456789012ull;
    MeterEntry mono;
    mono.meterId = 1;
    mono.channels = 1;
    mono.flags = kMeterFlagClip;
    mono.peakCdb[0] = -600;
    mono.rmsCdb[0] = -1200;
    mono.peakCdb[1] = mono.rmsCdb[1] = kMeterSilenceCdb; // unused channel keeps its default
    MeterEntry stereo;
    stereo.meterId = 0;
    stereo.peakCdb[0] = 0;
    stereo.peakCdb[1] = -100;
    stereo.rmsCdb[0] = -300;
    stereo.rmsCdb[1] = -400;
    p.entries = {mono, stereo};

    const auto bytes = encodeMeterPacket(p);
    CHECK(bytes.size() == kMeterHeaderBytes + 8 + 12);
    auto back = decodeMeterPacket(bytes);
    REQUIRE(back.has_value());
    CHECK(*back == p);
}

TEST_CASE("truncated or foreign meter packets are rejected", "[meter]") {
    MeterPacket p;
    p.entries.resize(3);
    auto bytes = encodeMeterPacket(p);
    bytes.pop_back();
    CHECK_FALSE(decodeMeterPacket(bytes).has_value());
    bytes = encodeMeterPacket(p);
    bytes[0] = 'X';
    CHECK_FALSE(decodeMeterPacket(bytes).has_value());
}

TEST_CASE("linear to centi-dB conversion", "[meter]") {
    CHECK(linearToCdb(1.0f) == 0);
    CHECK(linearToCdb(0.5f) == -602);
    CHECK(linearToCdb(0.0f) == kMeterSilenceCdb);
    CHECK(linearToCdb(1e-12f) == kMeterSilenceCdb);
    CHECK(linearToCdb(100.0f) == 2400);
}
