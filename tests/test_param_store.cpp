#include "dc/protocol/defaults.hpp"
#include "dc/protocol/param_store.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace dc::proto;

namespace {

ParamStore seeded(int trackCount = 3) {
    ParamStore s;
    auto c = globalDefaults();
    for (const auto& m : masterDefaults()) c.push_back(m);
    for (int n = 1; n <= trackCount; ++n) {
        for (const auto& t : trackDefaults(n)) c.push_back(t);
        c.push_back({"/track/" + std::to_string(n) + "/position", std::int64_t{n}});
    }
    s.publish(c);
    return s;
}

} // namespace

TEST_CASE("a valid set changes the tree and produces a delta", "[store]") {
    auto s = seeded();
    const auto rev = s.tree().rev();
    auto out = s.applySet(Set{5, {{"/track/1/volume_db", std::int64_t{-6}}, {"/track/1/mute", true}}}, "ui");
    CHECK(out.result.code == ErrorCode::Ok);
    CHECK(out.result.id == 5);
    REQUIRE(out.delta.has_value());
    CHECK(out.delta->rev == rev + 1);
    CHECK(out.delta->originId == 5);
    CHECK(out.delta->originClient == "ui");
    CHECK(out.delta->changes.size() == 2);
    CHECK(s.tree().getAs<double>("/track/1/volume_db") == -6.0); // normalized to float
    CHECK(s.tree().rev() == rev + 1);
}

TEST_CASE("setting the current value produces no delta", "[store]") {
    auto s = seeded();
    const auto rev = s.tree().rev();
    auto out = s.applySet(Set{1, {{"/track/1/volume_db", 0.0}}}, "ui");
    CHECK(out.result.code == ErrorCode::Ok);
    CHECK_FALSE(out.delta.has_value());
    CHECK(s.tree().rev() == rev);
}

TEST_CASE("sets are all-or-nothing", "[store]") {
    auto s = seeded();
    const auto before = s.tree();
    struct Case {
        Change bad;
        ErrorCode code;
    };
    const Case cases[] = {
        {{"/track/9/volume_db", 0.0}, ErrorCode::NotFound},
        {{"/track/1/volume_db", 20.0}, ErrorCode::OutOfRange},
        {{"/track/1/mute", std::int64_t{1}}, ErrorCode::TypeMismatch},
        {{"/capture/state", std::string("recording")}, ErrorCode::ReadOnly},
        {{"/track/1/Volume", 0.0}, ErrorCode::BadRequest},
    };
    for (const auto& c : cases) {
        INFO(c.bad.path);
        auto out = s.applySet(Set{1, {{"/track/2/volume_db", -3.0}, c.bad}}, "ui");
        CHECK(out.result.code == c.code);
        CHECK(out.result.path == c.bad.path);
        CHECK_FALSE(out.delta.has_value());
        CHECK(s.tree() == before);
    }
}

TEST_CASE("routing loops are rolled back", "[store]") {
    auto s = seeded();
    REQUIRE(s.applySet(Set{1, {{"/track/1/out/2/target", std::string("track/2")}}}, "ui").result.code ==
            ErrorCode::Ok);
    const auto before = s.tree();
    auto out = s.applySet(Set{2, {{"/track/3/volume_db", -1.0}, {"/track/2/out/2/target", std::string("track/1")}}},
                          "ui");
    CHECK(out.result.code == ErrorCode::Conflict);
    CHECK(s.tree() == before);
}

TEST_CASE("engine publish adds, changes and removes paths", "[store]") {
    auto s = seeded(2);
    const auto rev = s.tree().rev();
    auto d = s.publish({{"/capture/state", std::string("recording")}}, {"/track/2"});
    REQUIRE(d.has_value());
    CHECK(d->rev == rev + 1);
    CHECK(d->removed == std::vector<std::string>{"/track/2"});
    CHECK_FALSE(s.tree().contains("/track/2/volume_db"));
    CHECK(s.tree().getAs<std::string>("/capture/state") == "recording");

    CHECK_FALSE(s.publish({{"/capture/state", std::string("recording")}}).has_value()); // no-op
    CHECK_THROWS_AS(s.publish({{"/nope", true}}), std::invalid_argument);
    CHECK_THROWS_AS(s.publish({{"/capture/state", std::string("dancing")}}), std::invalid_argument);
}
