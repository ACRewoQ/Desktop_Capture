#include "dc/protocol/defaults.hpp"
#include "dc/protocol/fx.hpp"
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

TEST_CASE("effect parameters are checked against the slot's type", "[store][fx]") {
    auto s = seeded();
    s.publish({{"/track/1/fx/1/type", std::string("comp")},
               {"/track/1/fx/1/param/ratio", 4.0},
               {"/track/1/fx/1/editor", false},
               {"/master/fx/1/type", std::string("vst3:1a2b3c4d")},
               {"/master/fx/1/param/p42", 0.5},
               {"/master/fx/1/editor", false}});

    CHECK(s.applySet(Set{1, {{"/track/1/fx/1/param/ratio", 8.0}}}, "ui").result.code == ErrorCode::Ok);
    CHECK(s.applySet(Set{2, {{"/track/1/fx/1/param/ratio", 50.0}}}, "ui").result.code == ErrorCode::OutOfRange);
    CHECK(s.tree().getAs<double>("/track/1/fx/1/param/ratio") == 8.0); // rolled back
    CHECK(s.applySet(Set{3, {{"/master/fx/1/param/p42", 1.5}}}, "ui").result.code == ErrorCode::OutOfRange);
    CHECK(s.applySet(Set{4, {{"/master/fx/1/param/p42", std::int64_t{1}}}}, "ui").result.code == ErrorCode::Ok);
    CHECK(s.applySet(Set{5, {{"/track/1/fx/1/editor", true}}}, "ui").result.code == ErrorCode::Conflict);
    CHECK(s.applySet(Set{6, {{"/master/fx/1/editor", true}}}, "ui").result.code == ErrorCode::Ok);
    // a key the slot does not have is not in the tree
    CHECK(s.applySet(Set{7, {{"/track/1/fx/1/param/p1", 0.5}}}, "ui").result.code == ErrorCode::NotFound);
}

TEST_CASE("effect catalog is consistent", "[schema][fx]") {
    const auto& schema = schemaV1();
    for (const auto& t : builtinFx()) {
        INFO(t.type);
        CHECK(findBuiltinFx(t.type) == &t);
        CHECK_FALSE(isVst3Type(t.type));
        for (const auto& p : t.params) {
            INFO(p.key);
            CHECK(p.min < p.max);
            CHECK(p.defaultValue >= p.min);
            CHECK(p.defaultValue <= p.max);
            const auto path = "/track/1/fx/1/param/" + p.key;
            CHECK(isValidPath(path));
            CHECK(schema.find(path) != nullptr);
            CHECK(findFxParam(t, p.key) == &p);
        }
    }
    CHECK(vst3ParamKey(42) == "p42");
    CHECK(vst3ParamId("p42") == 42u);
    CHECK(vst3ParamId("p4294967295") == 4294967295u);
    CHECK_FALSE(vst3ParamId("p042"));
    CHECK_FALSE(vst3ParamId("ratio"));
    CHECK_FALSE(vst3ParamId("p"));
    auto fp = parseFxParamPath("/track/3/fx/2/param/ratio");
    REQUIRE(fp);
    CHECK(fp->slot == "/track/3/fx/2");
    CHECK(fp->key == "ratio");
    CHECK(parseFxParamPath("/master/fx/1/param/p7")->slot == "/master/fx/1");
    CHECK_FALSE(parseFxParamPath("/track/3/volume_db"));
    CHECK(schema.findMethod("/track/2/fx/3/params") != nullptr);
    CHECK(schema.findMethod("/master/fx/move") != nullptr);
    CHECK(schema.findMethod("/plugins/scan") != nullptr);
}
