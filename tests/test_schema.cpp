#include "dc/protocol/defaults.hpp"
#include "dc/protocol/profile.hpp"
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

TEST_CASE("control bindings (hotkeys and MIDI) are in the schema", "[schema]") {
    const auto& s = schemaV1();
    REQUIRE(s.find("/control/binding/128/trigger") != nullptr);
    CHECK(s.find("/control/binding/3/state")->access == Access::ReadOnly);
    CHECK(s.find("/control/midi/device/2/enabled")->access == Access::ReadWrite);
    const ParamSpec& action = *s.find("/control/binding/1/action");
    CHECK_FALSE(s.validate(action, std::string("record_toggle"), true));
    CHECK(s.validate(action, std::string("shutdown"), true).code == ErrorCode::OutOfRange);
    CHECK(s.findMethod("/control/binding/add") != nullptr);
    CHECK(s.findMethod("/control/learn/cancel") != nullptr);

    // bindings and MIDI devices are created by the engine, only the learn state is global
    std::set<std::string> global;
    for (const auto& c : globalDefaults()) global.insert(c.path);
    CHECK(global.count("/control/learn/binding"));
    CHECK(global.count("/control/midi/last/trigger"));
    CHECK_FALSE(global.count("/control/binding/1/trigger"));
    CHECK_FALSE(global.count("/control/midi/device/1/name"));
}

TEST_CASE("profiles and mixer snapshots are in the schema", "[schema]") {
    const auto& s = schemaV1();
    CHECK(s.find("/profile/current")->access == Access::ReadOnly);
    CHECK(s.find("/profile/autosave")->access == Access::ReadWrite);
    REQUIRE(s.find("/profile/list/64/name") != nullptr);
    REQUIRE(s.find("/snapshot/64/name") != nullptr);
    CHECK(s.find("/snapshot/last") != nullptr);
    for (const char* m : {"/profile/save", "/profile/load", "/profile/new", "/profile/rename", "/profile/remove",
                          "/snapshot/save", "/snapshot/recall", "/snapshot/remove"})
        CHECK(s.findMethod(m) != nullptr);
    CHECK_FALSE(s.validate(*s.find("/control/binding/1/action"), std::string("snapshot"), true));

    std::set<std::string> global;
    for (const auto& c : globalDefaults()) global.insert(c.path);
    CHECK(global.count("/profile/autosave"));
    CHECK(global.count("/snapshot/last"));
    CHECK_FALSE(global.count("/profile/list/1/name"));
    CHECK_FALSE(global.count("/snapshot/1/name"));
}

TEST_CASE("what a profile and a snapshot keep", "[schema]") {
    const auto& s = schemaV1();
    auto profile = [&](const char* path) { return inProfile(*s.find(path)); };
    auto snapshot = [&](const char* path) { return inSnapshot(*s.find(path)); };

    // settings and the structure they need
    for (const char* p : {"/track/3/volume_db", "/track/3/input/source", "/track/3/out/2/target", "/track/3/position",
                          "/track/3/fx/2/type", "/track/3/fx/2/param/p17", "/master/fx/1/type", "/system/block_size",
                          "/cue/target", "/capture/video/bitrate_kbps", "/recorder/format", "/sampler/bank/2/pad/5/file",
                          "/video/source/4/kind", "/video/scene/2/layer/3/position", "/video/mix/2/program_scene",
                          "/video/vcam/mix", "/lan/stream/1/codec"})
        CHECK(profile(p));
    // states, lists, PC-wide and separately stored settings
    for (const char* p : {"/track/3/meter_id", "/track/3/input/state", "/track/3/fx/2/status", "/track/3/fx/2/editor",
                          "/system/xruns", "/devices/input/1/id", "/capture/state", "/sampler/bank/1/pad/1/state",
                          "/video/source/1/state", "/video/devices/display/1/id", "/video/vcam/state", "/lan/name",
                          "/lan/stream/1/stream_id", "/plugins/extra_dirs", "/remote/paired/1/id",
                          "/control/binding/1/trigger", "/profile/autosave", "/profile/current", "/snapshot/1/name"})
        CHECK_FALSE(profile(p));

    for (const char* p : {"/track/3/volume_db", "/track/3/pan", "/track/3/mute", "/track/3/solo", "/track/3/lowcut/freq_hz",
                          "/track/3/send/2/level_db", "/track/3/fx/1/bypass", "/track/3/fx/1/param/threshold_db",
                          "/master/volume_db", "/master/highcut/enabled", "/cue/volume_db"})
        CHECK(snapshot(p));
    for (const char* p : {"/track/3/name", "/track/3/input/kind", "/track/3/out/1/target", "/track/3/send/1/target",
                          "/track/3/channels", "/track/3/cue", "/track/3/record/enabled", "/track/3/fx/1/type",
                          "/track/3/position", "/cue/target", "/video/mix/1/program_scene"})
        CHECK_FALSE(snapshot(p));
}

TEST_CASE("profile names are file names", "[schema]") {
    for (const char* n : {"標準", "Stream 2", "配信 (ゲーム)", "a.b", "con1", "comx"}) CHECK(isValidProfileName(n));
    for (const char* n : {"", " a", "a ", ".a", "a.", "a/b", "a\\b", "a:b", "a*", "a?", "a\"", "a<", "a>", "a|", "a\tb",
                          "con", "NUL", "nul.txt", "COM1", "lpt9"})
        CHECK_FALSE(isValidProfileName(n));
    CHECK(isValidProfileName(std::string(64, 'a')));
    CHECK_FALSE(isValidProfileName(std::string(65, 'a')));
    std::string jp;
    for (int k = 0; k < 64; ++k) jp += "あ";
    CHECK(isValidProfileName(jp)); // characters, not bytes
}

TEST_CASE("string values must be UTF-8", "[schema]") {
    CHECK(isValidUtf8(""));
    CHECK(isValidUtf8("C:\\Sounds\\kick.wav"));
    CHECK(isValidUtf8("\xe5\x8a\xb9\xe6\x9e\x9c\xe9\x9f\xb3.wav")); // 効果音
    CHECK(isValidUtf8("\xf0\x9f\x8e\xb5"));                           // U+1F3B5
    CHECK_FALSE(isValidUtf8("\x8c\xf8\x89\xca\x89\xb9.wav"));          // 効果音 in CP932
    CHECK_FALSE(isValidUtf8("\xe5\x8a"));                                // cut off
    CHECK_FALSE(isValidUtf8("\xc0\xaf"));                                // overlong "/"
    CHECK_FALSE(isValidUtf8("\xed\xa0\x80"));                            // surrogate
    CHECK_FALSE(isValidUtf8("\xf4\x90\x80\x80"));                        // above U+10FFFF
    const auto& s = schemaV1();
    const auto* spec = s.find("/sampler/bank/1/pad/1/file");
    REQUIRE(spec);
    CHECK(s.validate(*spec, Value{std::string("\x8c\xf8.wav")}, true).code == ErrorCode::TypeMismatch);
    CHECK_FALSE(s.validate(*spec, Value{std::string("\xe5\x8a\xb9.wav")}, true));
}
