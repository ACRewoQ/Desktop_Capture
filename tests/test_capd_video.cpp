#include "video_config.hpp"

#include "dc/protocol/defaults.hpp"
#include "dc/protocol/param_store.hpp"
#include "dc/protocol/video.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace dc;
using namespace dc::capd;
using Catch::Approx;

namespace {

proto::ParamStore seeded() {
    proto::ParamStore s;
    s.publish(proto::globalDefaults());
    return s;
}

std::int64_t callInt(proto::ParamStore& s, const std::string& method, proto::Args args) {
    auto o = proto::videoCall(s.tree(), method, args, proto::ClientRole::Local);
    REQUIRE(o.result.code == proto::ErrorCode::Ok);
    s.publish(o.changes, o.removed);
    return std::holds_alternative<std::int64_t>(o.result.value) ? std::get<std::int64_t>(o.result.value) : 0;
}

}  // namespace

TEST_CASE("the video config is read from the tree, layers back to front", "[capd][video]") {
    auto s = seeded();
    const auto cam = callInt(s, "/video/source/add", {{"kind", std::string("camera")}});
    callInt(s, "/video/scene/1/layer/add", {{"source", cam}});  // in front of the display
    s.publish({{"/video/scene/1/layer/2/x", 0.7},
               {"/video/scene/1/layer/2/fit", std::string("fill")},
               {"/video/scene/1/layer/2/chroma/enabled", true},
               {"/video/source/2/camera/id", std::string("cam-id")},
               {"/video/mix/1/background", std::int64_t{0xFF102030}},
               {"/video/mix/1/width", std::int64_t{1279}}});

    const auto cfg = videoConfigFromTree(s.tree());
    REQUIRE(cfg.sources.size() == 2);
    CHECK(cfg.sources.at(1).kind == "display");
    CHECK(cfg.sources.at(2).kind == "camera");
    CHECK(cfg.sources.at(2).cameraId == "cam-id");
    REQUIRE(cfg.scenes.at(1).layers.size() == 2);
    CHECK(cfg.scenes.at(1).layers[0].id == 1);  // display at the back
    CHECK(cfg.scenes.at(1).layers[1].id == 2);
    CHECK(cfg.scenes.at(1).layers[1].x == Approx(0.7));
    CHECK(cfg.scenes.at(1).layers[1].fit == "fill");
    CHECK(cfg.scenes.at(1).layers[1].chroma);
    const auto& mix = cfg.mixes.at(1);
    CHECK(mix.background == 0xFF102030u);
    CHECK(mix.width == 1278);  // even sizes for NV12
    CHECK(mix.programScene == 1);
    CHECK(mix.transition == "fade");
}

TEST_CASE("layers are fitted, filled, stretched and cropped", "[capd][video]") {
    LayerConfig l;  // whole canvas
    SECTION("fit keeps the aspect ratio inside the box") {
        const auto g = layoutLayer(l, 1000, 1000, 1920, 1080);
        CHECK(g.width == Approx(1080));
        CHECK(g.height == Approx(1080));
        CHECK(g.x == Approx(420));
        CHECK(g.y == Approx(0));
        CHECK(g.u0 == 0.0f);
        CHECK(g.u1 == 1.0f);
    }
    SECTION("fill covers the box and cuts the overflow from the source") {
        l.fit = "fill";
        const auto g = layoutLayer(l, 1000, 1000, 1920, 1080);
        CHECK(g.width == Approx(1920));
        CHECK(g.height == Approx(1080));
        CHECK(g.u0 == Approx(0));
        CHECK(g.u1 == Approx(1));
        CHECK(g.v0 == Approx(0.21875));  // 1080/1920 = 0.5625 of the height is visible
        CHECK(g.v1 == Approx(0.78125));
    }
    SECTION("stretch fills the box") {
        l.fit = "stretch";
        const auto g = layoutLayer(l, 1000, 1000, 1920, 1080);
        CHECK(g.width == Approx(1920));
        CHECK(g.height == Approx(1080));
    }
    SECTION("crop happens before fitting") {
        l.cropLeft = 0.25;
        l.cropRight = 0.25;  // 1920x1080 -> 960x1080 visible
        const auto g = layoutLayer(l, 1920, 1080, 1920, 1080);
        CHECK(g.u0 == Approx(0.25));
        CHECK(g.u1 == Approx(0.75));
        CHECK(g.width == Approx(960));
        CHECK(g.x == Approx(480));
    }
    SECTION("a box in the corner") {
        l.x = 0.75;
        l.y = 0.75;
        l.width = 0.25;
        l.height = 0.25;
        const auto g = layoutLayer(l, 1280, 720, 1920, 1080);
        CHECK(g.x == Approx(1440));
        CHECK(g.y == Approx(810));
        CHECK(g.width == Approx(480));
        CHECK(g.height == Approx(270));
    }
    SECTION("cropping everything leaves nothing to draw") {
        l.cropLeft = 0.6;
        l.cropRight = 0.4;
        CHECK(layoutLayer(l, 100, 100, 100, 100).empty());
    }
    SECTION("a source without size fills its box") {
        l.width = 0.5;
        const auto g = layoutLayer(l, 0, 0, 1920, 1080);
        CHECK(g.width == Approx(960));
        CHECK(g.height == Approx(1080));
    }
}

TEST_CASE("transitions follow the program scene", "[capd][video]") {
    MixConfig m;
    m.programScene = 1;
    m.transitionMs = 300;
    TransitionTracker t;

    auto s = t.update(m, 0);
    CHECK(s.to == 1);
    CHECK_FALSE(s.active());  // the first scene is shown without a transition

    m.programScene = 2;
    s = t.update(m, 1000);
    CHECK(s.from == 1);
    CHECK(s.to == 2);
    CHECK(s.progress == Approx(0));
    s = t.update(m, 1150);
    CHECK(s.progress == Approx(0.5));
    CHECK_FALSE(s.fromFrozen);

    // changed again halfway: fade from a frozen picture of the half-done transition
    m.programScene = 3;
    s = t.update(m, 1150);
    CHECK(s.fromFrozen);
    CHECK(s.freezeNow);
    CHECK(s.to == 3);
    s = t.update(m, 1200);
    CHECK(s.fromFrozen);
    CHECK_FALSE(s.freezeNow);
    s = t.update(m, 1450);
    CHECK_FALSE(s.active());
    CHECK(s.from == 3);
    CHECK_FALSE(s.fromFrozen);

    m.transition = "cut";
    m.programScene = 1;
    s = t.update(m, 2000);
    CHECK_FALSE(s.active());
    CHECK(s.to == 1);
}

TEST_CASE("a mix keeps the sources of its program, preview and outgoing scenes open", "[capd][video]") {
    VideoConfig cfg;
    for (int i = 1; i <= 4; ++i) cfg.sources[i].id = i;
    auto scene = [&](int id, std::vector<int> sources) {
        SceneConfig sc;
        sc.id = id;
        for (int src : sources) {
            LayerConfig l;
            l.source = src;
            sc.layers.push_back(l);
        }
        cfg.scenes[id] = sc;
    };
    scene(1, {1});
    scene(2, {2, 3});
    scene(3, {4, 9});  // 9 does not exist
    MixConfig m;
    m.programScene = 1;
    m.previewScene = 2;
    TransitionTracker::State t;
    t.to = 1;
    std::set<int> need;
    addNeededSources(cfg, m, t, need);
    CHECK(need == std::set<int>{1, 2, 3});

    t.from = 3;
    t.progress = 0.5;
    need.clear();
    addNeededSources(cfg, m, t, need);
    CHECK(need == std::set<int>{1, 2, 3, 4});
}

TEST_CASE("the next view of a mix shows its preview scene at once", "[capd][video]") {
    VideoConfig cfg;
    MixConfig m;
    m.id = 2;
    m.width = 1280;
    m.height = 720;
    m.programScene = 1;
    m.previewScene = 3;
    m.transition = "fade";
    m.transitionMs = 500;
    cfg.mixes[2] = m;

    MixConfig out;
    REQUIRE(mixForKey(cfg, 2, out));
    CHECK(out == m);
    REQUIRE(mixForKey(cfg, kNextViewKey + 2, out));
    CHECK(out.id == kNextViewKey + 2);
    CHECK(out.programScene == 3);
    CHECK(out.width == 1280);
    CHECK(out.height == 720);
    CHECK_FALSE(mixForKey(cfg, 1, out));
    CHECK_FALSE(mixForKey(cfg, kNextViewKey + 1, out));

    // a new next scene replaces the picture without a transition
    TransitionTracker t;
    auto s = t.update(nextSceneView(m), 0);
    CHECK(s.to == 3);
    m.previewScene = 1;
    s = t.update(nextSceneView(m), 100);
    CHECK_FALSE(s.active());
    CHECK(s.to == 1);
}

TEST_CASE("colours are split into components", "[capd][video]") {
    const auto c = argbToRgba(0x80FF0000);
    CHECK(c.a == Approx(128.0 / 255));
    CHECK(c.r == Approx(1));
    CHECK(c.g == Approx(0));
}
