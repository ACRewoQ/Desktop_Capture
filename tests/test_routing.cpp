#include "dc/protocol/routing.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace dc::proto;

namespace {

ParamTree tracks(int n) {
    ParamTree t;
    for (int i = 1; i <= n; ++i) {
        t.set("/track/" + std::to_string(i) + "/position", std::int64_t{i});
        t.set("/track/" + std::to_string(i) + "/out/1/target", std::string("master"));
        t.set("/track/" + std::to_string(i) + "/out/2/target", std::string());
        t.set("/track/" + std::to_string(i) + "/send/1/target", std::string());
    }
    t.set("/master/out/1/target", std::string("device/spk"));
    return t;
}

void route(ParamTree& t, int from, const char* slot, const std::string& target) {
    t.set("/track/" + std::to_string(from) + "/" + slot + "/target", target);
}

} // namespace

TEST_CASE("target strings parse", "[routing]") {
    CHECK(parseTarget("")->kind == RouteTarget::Kind::None);
    CHECK(parseTarget("master")->kind == RouteTarget::Kind::Master);
    CHECK(parseTarget("track/12")->track == 12);
    CHECK(parseTarget("device/{0.0.0.00000000}.{abc}")->id == "{0.0.0.00000000}.{abc}");
    CHECK(parseTarget("vmic/cable-a")->kind == RouteTarget::Kind::VirtualMic);
    CHECK(parseTarget("lan/studio-pc/1")->id == "studio-pc/1"); // parses; validateRouting accepts lan/1..8 only
    CHECK(lanStreamOf(*parseTarget("lan/3")) == 3);
    CHECK(lanStreamOf(*parseTarget("lan/studio-pc/1")) == 0);
    CHECK(lanStreamOf(*parseTarget("track/3")) == 0);
    CHECK_FALSE(parseTarget("track/0"));
    CHECK_FALSE(parseTarget("track/65"));
    CHECK_FALSE(parseTarget("track/x"));
    CHECK_FALSE(parseTarget("speaker/1"));
    CHECK_FALSE(parseTarget("device/"));
}

TEST_CASE("valid routing passes and gives a processing order", "[routing]") {
    auto t = tracks(4);
    route(t, 2, "out/1", "track/1");   // 2 feeds 1
    route(t, 3, "send/1", "track/1");  // 3 sends to 1
    route(t, 4, "out/2", "vmic/cable");
    CHECK_FALSE(validateRouting(t));
    const auto order = processingOrder(t);
    REQUIRE(order.size() == 4);
    auto pos = [&](int n) { return std::find(order.begin(), order.end(), n) - order.begin(); };
    CHECK(pos(2) < pos(1));
    CHECK(pos(3) < pos(1));
}

TEST_CASE("loops are rejected", "[routing]") {
    auto t = tracks(3);
    route(t, 1, "out/2", "track/2");
    route(t, 2, "out/2", "track/3");
    CHECK_FALSE(validateRouting(t));
    route(t, 3, "send/1", "track/1");
    const auto err = validateRouting(t);
    CHECK(err.code == ErrorCode::Conflict);
    CHECK(err.message.find("routing loop") != std::string::npos);
    CHECK(findRoutingCycle(t).has_value());
    CHECK(processingOrder(t).empty());
}

TEST_CASE("self routing, missing tracks and master restrictions", "[routing]") {
    auto t = tracks(2);
    route(t, 1, "out/2", "track/1");
    CHECK(validateRouting(t).code == ErrorCode::Conflict);

    t = tracks(2);
    route(t, 1, "out/2", "track/5");
    CHECK(validateRouting(t).code == ErrorCode::NotFound);

    t = tracks(2);
    t.set("/master/out/1/target", std::string("track/1"));
    CHECK(validateRouting(t).code == ErrorCode::Conflict);

    t = tracks(2);
    t.set("/master/out/1/target", std::string("lan/1"));
    CHECK_FALSE(validateRouting(t));

    for (const char* bad : {"lan/peer", "lan/0", "lan/9", "lan/01", "lan/1x"}) {
        t = tracks(2);
        t.set("/master/out/1/target", std::string(bad));
        CHECK(validateRouting(t).code == ErrorCode::OutOfRange);
        t = tracks(2);
        route(t, 1, "send/1", bad);
        CHECK(validateRouting(t).code == ErrorCode::OutOfRange);
    }
    t = tracks(2);
    route(t, 2, "out/3", "lan/8");
    CHECK_FALSE(validateRouting(t));

    t = tracks(2);
    route(t, 1, "send/1", "master");
    CHECK(validateRouting(t).code == ErrorCode::Conflict);

    t = tracks(2);
    route(t, 1, "out/2", "bogus");
    CHECK(validateRouting(t).code == ErrorCode::OutOfRange);
}
