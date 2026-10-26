#include "dc/protocol/control.hpp"
#include "dc/protocol/defaults.hpp"
#include "dc/protocol/param_store.hpp"
#include "dc/protocol/profile.hpp"
#include "dc/protocol/video.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace dc::proto;

namespace {

// A fresh engine's tree: the global defaults, the master and tracks 1..n.
ParamTree freshTree(int tracks = 4) {
    ParamStore store;
    std::vector<Change> c = globalDefaults();
    for (auto& m : masterDefaults()) c.push_back(m);
    for (int n = 1; n <= tracks; ++n) {
        for (auto& t : trackDefaults(n)) c.push_back(t);
        c.push_back({"/track/" + std::to_string(n) + "/position", std::int64_t{n}});
    }
    c.push_back({"/mixer/track_count", std::int64_t{tracks}});
    store.publish(c);
    return store.tree();
}

ParamTree applied(ParamTree tree, const RestorePlan& plan) {
    for (const auto& r : plan.removed) tree.removeSubtree(r);
    for (const auto& c : plan.changes) tree.set(c.path, c.value);
    return tree;
}

const Value* find(const std::vector<Change>& v, const std::string& path) {
    for (const auto& c : v)
        if (c.path == path) return &c.value;
    return nullptr;
}

} // namespace

TEST_CASE("profile file round trip", "[profile]") {
    ProfileData p;
    p.values = {{"/track/1/name", std::string("Mic\twith\\tab\nand line")},
                {"/track/1/volume_db", -6.25},
                {"/track/1/mute", true},
                {"/track/1/position", std::int64_t{1}},
                {"/capture/directory", std::string("D:\\録画")},
                {"/x/nil", Value{}}};
    p.devices = {{"{0.0.1.00000000}.{abc}", "Line (Steinberg UR22)", true}, {"out-1", "スピーカー", false}};
    p.fxState["/track/1/fx/2"] = std::string("\0\x01\xff state", 9);
    p.snapshots = {{3, "Talk", 1761480000, {{"/track/1/volume_db", -3.0}, {"/track/1/fx/1/type", std::string("comp")}}},
                   {1, "", 0, {}}};
    const auto text = writeProfile(p);
    int skipped = -1;
    std::string err;
    const auto back = readProfile(text, &err, &skipped);
    REQUIRE(back);
    CHECK(skipped == 0);
    CHECK(*back == p);
    CHECK(std::get<double>(back->values[1].value) == -6.25); // exact doubles

    // CRLF (edited with Notepad) still reads
    std::string crlf;
    for (char c : text) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    CHECK(readProfile(crlf));
}

TEST_CASE("broken profile files", "[profile]") {
    std::string err;
    CHECK_FALSE(readProfile("", &err));
    CHECK_FALSE(readProfile("hello\nend\n", &err));
    CHECK_FALSE(readProfile("dcprofile\t2\nend\n", &err));
    CHECK(err.find("新しい") != std::string::npos);
    CHECK_FALSE(readProfile("dcprofile\t1\nv\t/track/1/mute\tb\t1\n", &err)); // cut short
    int skipped = 0;
    const auto p = readProfile("dcprofile\t1\nv\t/a\tb\t2\nv\t/b\ti\tx\nfx\t/s\t!!\nsv\t9\t/a\tb\t1\nfuture\tthing\n"
                               "v\t/track/1/mute\tb\t1\nend\n",
                               &err, &skipped);
    REQUIRE(p);
    CHECK(skipped == 4);
    REQUIRE(p->values.size() == 1);
    CHECK(p->values[0].path == "/track/1/mute");
}

TEST_CASE("base64", "[profile]") {
    for (const std::string& s : {std::string(), std::string("f"), std::string("fo"), std::string("foo"),
                                std::string("foob"), std::string("\0\xfe\xff", 3)}) {
        const auto e = base64Encode(s);
        CHECK(e.size() % 4 == 0);
        CHECK(base64Decode(e) == s);
    }
    CHECK(base64Encode("foobar") == "Zm9vYmFy");
    CHECK_FALSE(base64Decode("Zm9"));
    CHECK_FALSE(base64Decode("Zm=v"));
    CHECK_FALSE(base64Decode("Zm9v!A=="));
}

TEST_CASE("profile values and device references", "[profile]") {
    auto tree = freshTree(2);
    tree.set("/track/1/input/kind", std::string("device"));
    tree.set("/track/1/input/source", std::string("in-a"));
    tree.set("/track/2/input/kind", std::string("process"));
    tree.set("/track/2/input/source", std::string("chrome.exe"));
    tree.set("/master/out/1/target", std::string("device/out-b"));
    tree.set("/track/2/send/1/target", std::string("vmic/cable"));
    tree.set("/cue/target", std::string("device/out-b"));
    tree.set("/system/clock_device", std::string("out-c"));
    tree.set("/devices/input/1/id", std::string("in-a"));
    tree.set("/track/1/fx/1/type", std::string("comp"));
    tree.set("/track/1/fx/1/status", std::string("ok"));
    tree.set("/track/1/fx/1/param/ratio", 4.0);

    const auto refs = deviceRefs(tree);
    CHECK(refs == std::vector<std::string>{"cable", "in-a", "out-b", "out-c"});

    const auto values = profileValues(tree);
    CHECK(find(values, "/track/1/position"));
    CHECK(find(values, "/track/1/fx/1/type"));
    CHECK(find(values, "/track/1/fx/1/param/ratio"));
    CHECK_FALSE(find(values, "/track/1/fx/1/status"));
    CHECK_FALSE(find(values, "/track/1/meter_id"));
    CHECK_FALSE(find(values, "/devices/input/1/id"));
    CHECK_FALSE(find(values, "/capture/state"));

    const auto snap = snapshotValues(tree);
    CHECK(find(snap, "/track/1/volume_db"));
    CHECK(find(snap, "/track/1/fx/1/type"));
    CHECK(find(snap, "/track/1/fx/1/param/ratio"));
    CHECK_FALSE(find(snap, "/track/1/name"));
}

TEST_CASE("restore rebuilds the saved tree", "[profile]") {
    // the saved state: tracks 2, 5 (5 first on screen), master and track 2 with effects, two mixes, a scene
    auto saved = freshTree(0);
    auto addTrack = [&](int n, int pos) {
        for (const auto& c : trackDefaults(n)) saved.set(c.path, c.value);
        saved.set("/track/" + std::to_string(n) + "/position", std::int64_t{pos});
    };
    addTrack(5, 1);
    addTrack(2, 2);
    saved.set("/track/2/name", std::string("Game"));
    saved.set("/track/2/volume_db", -12.0);
    saved.set("/track/2/out/1/target", std::string("track/5"));
    saved.set("/track/2/fx/1/type", std::string("eq4"));
    saved.set("/track/2/fx/1/bypass", true);
    saved.set("/track/2/fx/2/type", std::string("vst3:ABC"));
    saved.set("/track/2/fx/2/param/p7", 0.25);
    saved.set("/master/fx/1/type", std::string("limiter"));
    for (const auto& c : videoMixDefaults(3)) saved.set(c.path, c.value);
    saved.set("/video/mix/3/name", std::string("Sub"));
    saved.set("/video/mix/1/program_scene", std::int64_t{1});
    saved.set("/capture/mix", std::int64_t{3});
    saved.set("/capture/video/bitrate_kbps", std::int64_t{40000});
    saved.set("/sampler/bank/2/pad/3/file", std::string("C:\\se\\pon.wav"));
    const auto values = profileValues(saved);

    // restore onto a different running tree (4 tracks, an extra mix and source)
    auto current = freshTree(4);
    current.set("/track/4/fx/1/type", std::string("comp"));
    for (const auto& c : videoMixDefaults(2)) current.set(c.path, c.value);
    current.set("/capture/video/bitrate_kbps", std::int64_t{9000});

    const auto plan = planRestore(current, values);
    CHECK(plan.skipped == 0);
    const auto t = applied(current, plan);
    CHECK_FALSE(t.contains("/track/1/position"));
    CHECK_FALSE(t.contains("/track/4/fx/1/type"));
    CHECK(t.getAs<std::int64_t>("/track/5/position") == 1);
    CHECK(t.getAs<std::int64_t>("/track/2/position") == 2);
    CHECK(t.getAs<std::int64_t>("/mixer/track_count") == 2);
    CHECK(t.getAs<std::int64_t>("/track/2/meter_id") == 2); // the engine's values come back with the track
    CHECK(t.getAs<std::string>("/track/2/input/state") == "none");
    CHECK(t.getAs<std::string>("/track/2/name") == "Game");
    CHECK(t.getAs<double>("/track/2/volume_db") == -12.0);
    CHECK(t.getAs<std::string>("/track/2/out/1/target") == "track/5");
    CHECK_FALSE(t.contains("/track/2/fx/1/type")); // effect slots are left to the engine
    CHECK(t.contains("/video/mix/3/name"));
    CHECK_FALSE(t.contains("/video/mix/2/name"));
    CHECK(t.getAs<std::int64_t>("/capture/mix") == 3);
    CHECK(t.getAs<std::int64_t>("/capture/video/bitrate_kbps") == 40000);
    CHECK(t.getAs<std::string>("/sampler/bank/2/pad/3/file") == "C:\\se\\pon.wav");
    CHECK(t.contains("/video/source/1/kind"));
    CHECK(t.getAs<std::int64_t>("/video/scene/1/layer/1/source") == 1);
    CHECK(t.contains("/video/source/1/state"));

    REQUIRE(plan.fx.count(2));
    REQUIRE(plan.fx.at(2).size() == 2);
    CHECK(plan.fx.at(2)[0].type == "eq4");
    CHECK(plan.fx.at(2)[1].fileSlot == "/track/2/fx/2");
    CHECK(plan.fx.at(2)[1].values == std::vector<std::pair<std::string, Value>>{{"/param/p7", 0.25}});
    REQUIRE(plan.fx.count(0));
    CHECK(plan.fx.at(0)[0].type == "limiter");
    CHECK_FALSE(plan.fx.count(4));

    // restoring the same values again changes nothing
    const auto again = applied(t, planRestore(t, values));
    CHECK(again.values() == t.values());
}

TEST_CASE("restore skips what does not fit", "[profile]") {
    const auto current = freshTree(2);
    std::vector<Change> values = profileValues(current);
    values.push_back({"/track/1/volume_db", std::string("loud")});      // wrong type
    values.push_back({"/track/2/pan", 7.0});                           // out of range
    values.push_back({"/track/9/name", std::string("no position")});   // track without its position
    values.push_back({"/track/1/out/1/target", std::string("track/1")}); // routes to itself
    values.push_back({"/capture/state", std::string("recording")});    // not a profile value
    values.push_back({"/nope", true});                                 // not in the schema
    values.push_back({"/track/1/fx/1/type", std::string("wah")});      // unknown effect
    values.push_back({"/track/2/fx/1/type", std::string("comp")});
    values.push_back({"/track/2/fx/1/param/ratio", 999.0});            // out of the effect's range
    values.push_back({"/track/2/fx/1/param/nope", 1.0});               // not a parameter of comp
    values.push_back({"/video/scene/1/layer/1/source", std::int64_t{40}}); // no such source
    const auto plan = planRestore(current, values);
    CHECK(plan.skipped == 10);
    const auto t = applied(current, plan);
    CHECK(t.getAs<double>("/track/1/volume_db") == 0.0);
    CHECK(t.getAs<std::string>("/track/1/out/1/target") == "master");
    CHECK_FALSE(t.contains("/track/9/name"));
    CHECK(t.getAs<std::int64_t>("/video/scene/1/layer/1/source") == 0); // the saved 1 was replaced by 40
    REQUIRE(plan.fx.count(2));
    CHECK(plan.fx.at(2)[0].values.empty());
    CHECK_FALSE(plan.fx.count(1));
}

TEST_CASE("snapshot recall", "[profile]") {
    auto tree = freshTree(3);
    tree.set("/track/1/fx/1/type", std::string("comp"));
    tree.set("/track/1/fx/1/param/ratio", 4.0);
    tree.set("/track/2/fx/1/type", std::string("gate"));
    tree.set("/track/2/fx/1/param/threshold_db", -50.0);
    tree.set("/track/1/volume_db", -10.0);
    tree.set("/track/3/mute", true);
    const auto snap = snapshotValues(tree);

    // later: faders moved, track 3 removed, track 2's slot holds another effect now, names changed
    tree.set("/track/1/volume_db", 0.0);
    tree.set("/track/1/fx/1/param/ratio", 2.0);
    tree.set("/track/1/name", std::string("Renamed"));
    tree.removeSubtree("/track/3");
    tree.set("/track/2/fx/1/type", std::string("comp"));
    tree.set("/track/2/fx/1/param/threshold_db", -20.0);

    const auto set = planRecall(tree, snap);
    CHECK(find(set, "/track/1/volume_db"));
    CHECK(*find(set, "/track/1/fx/1/param/ratio") == Value{4.0});
    CHECK_FALSE(find(set, "/track/2/fx/1/param/threshold_db")); // not the same effect any more
    CHECK_FALSE(find(set, "/track/3/mute"));
    CHECK_FALSE(find(set, "/track/1/fx/1/type"));
    CHECK_FALSE(find(set, "/track/2/volume_db")); // unchanged values are left out
    for (const auto& c : set) tree.set(c.path, c.value);
    CHECK(tree.getAs<std::string>("/track/1/name") == "Renamed");
}

TEST_CASE("snapshot bindings", "[profile][control]") {
    ParamTree tree;
    tree.set("/snapshot/2/name", std::string("Talk"));
    ctl::Binding b;
    b.trigger = "key:ctrl+f1";
    b.action = "snapshot";
    b.target = "2";
    CHECK(ctl::checkBinding(tree, b).state == "ok");
    b.target = "3";
    CHECK(ctl::checkBinding(tree, b).state == "invalid");
    b.target = "65";
    CHECK(ctl::checkBinding(tree, b).state == "invalid");
    b.target = "";
    CHECK(ctl::checkBinding(tree, b).state == "invalid");
}
