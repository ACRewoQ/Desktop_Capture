#include "dc/protocol/defaults.hpp"
#include "dc/protocol/schema.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace dc::proto;

TEST_CASE("path syntax", "[schema]") {
    CHECK(isValidPath("/track/1/volume_db"));
    CHECK(isValidPath("/track/64/fx/2/param/cutoff.hz"));
    CHECK_FALSE(isValidPath(""));
    CHECK_FALSE(isValidPath("/"));
    CHECK_FALSE(isValidPath("track/1"));
    CHECK_FALSE(isValidPath("/track/1/"));
    CHECK_FALSE(isValidPath("/track//1"));
    CHECK_FALSE(isValidPath("/track/01/volume_db"));
    CHECK_FALSE(isValidPath("/track/0/volume_db"));
    CHECK_FALSE(isValidPath("/Track/1"));
    CHECK_FALSE(isValidPath("/track/1/vol db"));
}

TEST_CASE("index segments map to patterns", "[schema]") {
    CHECK(Schema::patternOf("/track/12/out/3/target") == "/track/#/out/#/target");
    CHECK(Schema::patternOf("/master/volume_db") == "/master/volume_db");
    const auto& s = schemaV1();
    REQUIRE(s.find("/track/12/volume_db") != nullptr);
    REQUIRE(s.find("/track/3/fx/1/param/threshold_db") != nullptr); // wildcard
    CHECK(s.find("/track/3/unknown") == nullptr);
    CHECK(s.findMethod("/track/5/fx/add") != nullptr);
    CHECK(s.findMethod("/mixer/track/add") != nullptr);
}

TEST_CASE("validation of types, ranges, choices and access", "[schema]") {
    const auto& s = schemaV1();
    const ParamSpec& vol = *s.find("/track/1/volume_db");
    CHECK_FALSE(s.validate(vol, -6.0, true));
    CHECK_FALSE(s.validate(vol, std::int64_t{-6}, true)); // ints accepted for floats
    CHECK(s.validate(vol, 13.0, true).code == ErrorCode::OutOfRange);
    CHECK(s.validate(vol, std::string("loud"), true).code == ErrorCode::TypeMismatch);
    CHECK(Schema::normalize(vol, std::int64_t{-6}) == Value{-6.0});

    const ParamSpec& ch = *s.find("/track/1/channels");
    CHECK_FALSE(s.validate(ch, std::int64_t{1}, true));
    CHECK(s.validate(ch, std::int64_t{3}, true).code == ErrorCode::OutOfRange);
    CHECK(s.validate(ch, 1.0, true).code == ErrorCode::TypeMismatch); // floats not accepted for ints

    const ParamSpec& codec = *s.find("/capture/video/codec");
    CHECK_FALSE(s.validate(codec, std::string("hevc"), true));
    CHECK(s.validate(codec, std::string("vp9"), true).code == ErrorCode::OutOfRange);

    const ParamSpec& state = *s.find("/capture/state");
    CHECK(s.validate(state, std::string("idle"), true).code == ErrorCode::ReadOnly);
    CHECK_FALSE(s.validate(state, std::string("idle"), false)); // engine may write it
}

TEST_CASE("every schema default passes its own validation", "[schema]") {
    const auto& s = schemaV1();
    for (const auto& spec : s.params()) {
        INFO(spec.pattern);
        CHECK_FALSE(s.validate(spec, spec.defaultValue, false));
    }
}

TEST_CASE("default parameter sets are complete and valid", "[schema]") {
    const auto& s = schemaV1();
    auto check = [&](const std::vector<Change>& changes) {
        std::set<std::string> seen;
        for (const auto& c : changes) {
            INFO(c.path);
            CHECK(isValidPath(c.path));
            const ParamSpec* spec = s.find(c.path);
            REQUIRE(spec != nullptr);
            CHECK_FALSE(s.validate(*spec, c.value, false));
            CHECK(seen.insert(c.path).second); // no duplicates
        }
        return seen;
    };
    const auto track = check(trackDefaults(7));
    CHECK(track.count("/track/7/out/8/target"));
    CHECK(track.count("/track/7/send/8/pre_fader"));
    CHECK_FALSE(track.count("/track/7/position")); // assigned by the owner
    CHECK_FALSE(track.count("/track/7/out/9/target"));
    for (const auto& c : trackDefaults(7))
        if (c.path == "/track/7/out/1/target") CHECK(c.value == Value{std::string("master")});

    const auto master = check(masterDefaults());
    CHECK(master.count("/master/out/1/target"));
    CHECK_FALSE(master.count("/master/solo"));

    const auto global = check(globalDefaults());
    CHECK(global.count("/capture/audio/8/codec"));
    CHECK(global.count("/video/mix/1/name"));
    CHECK_FALSE(global.count("/devices/input/1/id"));
    CHECK(global.count("/recorder/format"));
    CHECK(global.count("/sampler/bank/8/pad/16/file"));
    CHECK_FALSE(global.count("/sampler/bank/9/name"));
    CHECK_FALSE(global.count("/sampler/bank/1/pad/17/file"));
    for (const auto& c : globalDefaults())
        if (c.path == "/sampler/bank/3/name") CHECK(c.value == Value{std::string("Bank 3")});
}
