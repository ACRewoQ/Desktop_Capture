#include "dc/protocol/defaults.hpp"
#include "dc/protocol/param_store.hpp"
#include "dc/protocol/video.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace dc::proto;

namespace {

ParamStore seeded() {
    ParamStore s;
    s.publish(globalDefaults());
    s.publish(videoDerived(s.tree()));
    return s;
}

// Runs a video method like the control server does; returns the result.
Result call(ParamStore& s, const std::string& method, Args args = {}, ClientRole role = ClientRole::Local) {
    auto o = videoCall(s.tree(), method, args, role);
    if (o.result.code == ErrorCode::Ok) {
        if (!o.changes.empty() || !o.removed.empty()) s.publish(o.changes, o.removed);
        s.publish(videoDerived(s.tree()));
    }
    return o.result;
}

std::int64_t asInt(const Result& r) { return std::get<std::int64_t>(r.value); }

} // namespace

TEST_CASE("the start tree shows the primary display on mix 1", "[video]") {
    auto s = seeded();
    const auto& t = s.tree();
    CHECK(t.getAs<std::string>("/video/source/1/kind") == "display");
    CHECK(t.getAs<std::string>("/video/source/1/display/id") == "");
    CHECK(t.getAs<std::int64_t>("/video/scene/1/position") == 1);
    CHECK(t.getAs<std::int64_t>("/video/scene/1/layer/1/source") == 1);
    CHECK(t.getAs<double>("/video/scene/1/layer/1/width") == 1.0);
    CHECK(t.getAs<std::int64_t>("/video/mix/1/program_scene") == 1);
    CHECK(t.getAs<std::int64_t>("/video/mix/1/preview_scene") == 1);
    CHECK(t.getAs<bool>("/video/mix/1/active") == true);  // /capture/mix and /video/preview/mix = 1
    CHECK_FALSE(t.contains("/video/mix/2/width"));
    CHECK(videoMixes(t) == std::vector<int>{1});
    CHECK(videoSources(t) == std::vector<int>{1});
}

TEST_CASE("sources are added with the lowest free number and removed with their layers", "[video]") {
    auto s = seeded();
    const auto cam = call(s, "/video/source/add", {{"kind", std::string("camera")}});
    REQUIRE(cam.code == ErrorCode::Ok);
    CHECK(asInt(cam) == 2);
    CHECK(s.tree().getAs<std::string>("/video/source/2/name") == "Camera 2");
    CHECK(s.tree().getAs<std::string>("/video/source/2/state") == "none");
    const auto text = call(s, "/video/source/add", {{"kind", std::string("text")}, {"name", std::string("Title")}});
    CHECK(asInt(text) == 3);
    CHECK(s.tree().getAs<std::string>("/video/source/3/name") == "Title");

    CHECK(call(s, "/video/source/add", {{"kind", std::string("hologram")}}).code == ErrorCode::OutOfRange);
    CHECK(call(s, "/video/source/add").code == ErrorCode::OutOfRange);

    // camera on top of the display in scene 1
    const auto layer = call(s, "/video/scene/1/layer/add", {{"source", std::int64_t{2}}});
    REQUIRE(layer.code == ErrorCode::Ok);
    CHECK(asInt(layer) == 2);
    CHECK(videoLayersByPosition(s.tree(), 1) == std::vector<int>{2, 1});

    REQUIRE(call(s, "/video/source/remove", {{"source", std::int64_t{2}}}).code == ErrorCode::Ok);
    CHECK_FALSE(s.tree().contains("/video/source/2/kind"));
    CHECK_FALSE(s.tree().contains("/video/scene/1/layer/2/position"));
    CHECK(s.tree().getAs<std::int64_t>("/video/scene/1/layer/1/position") == 1);
    CHECK(call(s, "/video/source/remove", {{"source", std::int64_t{2}}}).code == ErrorCode::NotFound);

    // the freed number is reused
    CHECK(asInt(call(s, "/video/source/add", {{"kind", std::string("image")}})) == 2);
}

TEST_CASE("the source limit is enforced", "[video]") {
    auto s = seeded();
    for (int i = 2; i <= kMaxVideoSources; ++i)
        REQUIRE(call(s, "/video/source/add", {{"kind", std::string("color")}}).code == ErrorCode::Ok);
    CHECK(call(s, "/video/source/add", {{"kind", std::string("color")}}).code == ErrorCode::LimitReached);
}

TEST_CASE("scenes keep a dense position order", "[video]") {
    auto s = seeded();
    CHECK(asInt(call(s, "/video/scene/add", {{"name", std::string("Camera only")}})) == 2);
    CHECK(asInt(call(s, "/video/scene/add", {{"position", std::int64_t{1}}})) == 3);
    CHECK(videoScenesByPosition(s.tree()) == std::vector<int>{3, 1, 2});
    CHECK(s.tree().getAs<std::string>("/video/scene/2/name") == "Camera only");
    CHECK(s.tree().getAs<std::string>("/video/scene/3/name") == "Scene 3");

    REQUIRE(call(s, "/video/scene/move", {{"scene", std::int64_t{3}}, {"position", std::int64_t{3}}}).code == ErrorCode::Ok);
    CHECK(videoScenesByPosition(s.tree()) == std::vector<int>{1, 2, 3});
    CHECK(call(s, "/video/scene/move", {{"scene", std::int64_t{3}}, {"position", std::int64_t{4}}}).code ==
          ErrorCode::OutOfRange);
    CHECK(call(s, "/video/scene/add", {{"position", std::int64_t{9}}}).code == ErrorCode::OutOfRange);

    // removing the program scene leaves the mix without one
    REQUIRE(call(s, "/video/scene/remove", {{"scene", std::int64_t{1}}}).code == ErrorCode::Ok);
    CHECK(videoScenesByPosition(s.tree()) == std::vector<int>{2, 3});
    CHECK(s.tree().getAs<std::int64_t>("/video/scene/2/position") == 1);
    CHECK(s.tree().getAs<std::int64_t>("/video/mix/1/program_scene") == 0);
    CHECK(s.tree().getAs<std::int64_t>("/video/mix/1/preview_scene") == 0);
    CHECK(s.tree().pathsUnder("/video/scene/1").empty());
}

TEST_CASE("layers stack front to back and can be reordered", "[video]") {
    auto s = seeded();
    call(s, "/video/source/add", {{"kind", std::string("camera")}});
    call(s, "/video/source/add", {{"kind", std::string("text")}});
    // default: new layers go to the front
    CHECK(asInt(call(s, "/video/scene/1/layer/add", {{"source", std::int64_t{2}}, {"name", std::string("Cam")}})) == 2);
    // explicit position: behind everything
    CHECK(asInt(call(s, "/video/scene/1/layer/add", {{"source", std::int64_t{3}}, {"position", std::int64_t{3}}})) == 3);
    CHECK(videoLayersByPosition(s.tree(), 1) == std::vector<int>{2, 1, 3});
    CHECK(s.tree().getAs<std::string>("/video/scene/1/layer/2/name") == "Cam");
    CHECK(s.tree().getAs<std::string>("/video/scene/1/layer/2/fit") == "fit");
    CHECK(s.tree().getAs<bool>("/video/scene/1/layer/2/visible") == true);

    REQUIRE(call(s, "/video/scene/1/layer/move", {{"layer", std::int64_t{3}}, {"position", std::int64_t{1}}}).code ==
            ErrorCode::Ok);
    CHECK(videoLayersByPosition(s.tree(), 1) == std::vector<int>{3, 2, 1});

    REQUIRE(call(s, "/video/scene/1/layer/remove", {{"layer", std::int64_t{2}}}).code == ErrorCode::Ok);
    CHECK(videoLayersByPosition(s.tree(), 1) == std::vector<int>{3, 1});
    CHECK(s.tree().getAs<std::int64_t>("/video/scene/1/layer/1/position") == 2);

    CHECK(call(s, "/video/scene/1/layer/add", {{"source", std::int64_t{9}}}).code == ErrorCode::NotFound);
    CHECK(call(s, "/video/scene/7/layer/add", {{"source", std::int64_t{1}}}).code == ErrorCode::NotFound);
    CHECK(call(s, "/video/scene/1/layer/remove", {{"layer", std::int64_t{2}}}).code == ErrorCode::NotFound);
    // a layer without a source is allowed (picked later)
    CHECK(call(s, "/video/scene/1/layer/add").code == ErrorCode::Ok);
}

TEST_CASE("mixes are limited to four and removing one clears the outputs that used it", "[video]") {
    auto s = seeded();
    CHECK(asInt(call(s, "/video/mix/add")) == 2);
    CHECK(s.tree().getAs<std::string>("/video/mix/2/name") == "Mix B");
    CHECK(s.tree().getAs<bool>("/video/mix/2/active") == false);
    CHECK(asInt(call(s, "/video/mix/add", {{"name", std::string("Camera")}})) == 3);
    CHECK(asInt(call(s, "/video/mix/add")) == 4);
    CHECK(call(s, "/video/mix/add").code == ErrorCode::LimitReached);

    REQUIRE(s.applySet(Set{1, {{"/video/vcam/mix", std::int64_t{2}}}}, "ui").result.code == ErrorCode::Ok);
    s.publish(videoDerived(s.tree()));
    CHECK(s.tree().getAs<bool>("/video/mix/2/active") == true);

    REQUIRE(call(s, "/video/mix/remove", {{"mix", std::int64_t{2}}}).code == ErrorCode::Ok);
    CHECK(s.tree().getAs<std::int64_t>("/video/vcam/mix") == 0);
    CHECK(s.tree().pathsUnder("/video/mix/2").empty());
    CHECK(videoMixes(s.tree()) == std::vector<int>{1, 3, 4});
    CHECK(asInt(call(s, "/video/mix/add")) == 2);
}

TEST_CASE("take swaps preview and program", "[video]") {
    auto s = seeded();
    call(s, "/video/scene/add");
    REQUIRE(s.applySet(Set{1, {{"/video/mix/1/preview_scene", std::int64_t{2}}}}, "ui").result.code == ErrorCode::Ok);
    REQUIRE(call(s, "/video/mix/1/take").code == ErrorCode::Ok);
    CHECK(s.tree().getAs<std::int64_t>("/video/mix/1/program_scene") == 2);
    CHECK(s.tree().getAs<std::int64_t>("/video/mix/1/preview_scene") == 1);
    CHECK(call(s, "/video/mix/3/take").code == ErrorCode::NotFound);
}

TEST_CASE("references to missing sources, scenes and mixes are rejected", "[video]") {
    auto s = seeded();
    auto set = [&](const std::string& path, Value v) { return s.applySet(Set{1, {{path, std::move(v)}}}, "ui").result.code; };
    CHECK(set("/video/scene/1/layer/1/source", std::int64_t{5}) == ErrorCode::NotFound);
    CHECK(set("/video/scene/1/layer/1/source", std::int64_t{0}) == ErrorCode::Ok);
    CHECK(set("/video/scene/1/layer/1/source", std::int64_t{1}) == ErrorCode::Ok);
    CHECK(set("/video/mix/1/program_scene", std::int64_t{2}) == ErrorCode::NotFound);
    CHECK(set("/video/mix/1/preview_scene", std::int64_t{2}) == ErrorCode::NotFound);
    CHECK(set("/capture/mix", std::int64_t{3}) == ErrorCode::NotFound);
    CHECK(set("/video/monitor/mix", std::int64_t{1}) == ErrorCode::Ok);
    CHECK(set("/video/mix/1/background", std::int64_t{0x80FF0000}) == ErrorCode::Ok);
    CHECK(set("/video/scene/1/layer/1/crop/left", 1.5) == ErrorCode::OutOfRange);
    CHECK(set("/video/source/1/kind", std::string("camera")) == ErrorCode::ReadOnly);
    CHECK(set("/video/scene/1/layer/1/position", std::int64_t{1}) == ErrorCode::ReadOnly);
}

TEST_CASE("capd reports source states and device lists", "[video]") {
    auto s = seeded();
    const Args ok = {{"source", std::int64_t{1}}, {"state", std::string("ok")}, {"width", std::int64_t{2560}}, {"height", std::int64_t{1440}}};
    CHECK(call(s, "/video/source/report", ok, ClientRole::Local).code == ErrorCode::Unauthorized);
    REQUIRE(call(s, "/video/source/report", ok, ClientRole::Service).code == ErrorCode::Ok);
    CHECK(s.tree().getAs<std::string>("/video/source/1/state") == "ok");
    CHECK(s.tree().getAs<std::int64_t>("/video/source/1/width") == 2560);

    REQUIRE(call(s, "/video/source/report",
                 {{"source", std::int64_t{1}}, {"state", std::string("error")}, {"error", std::string("access denied")}},
                 ClientRole::Service)
                .code == ErrorCode::Ok);
    CHECK(s.tree().getAs<std::string>("/video/source/1/error") == "access denied");
    CHECK(call(s, "/video/source/report", {{"source", std::int64_t{1}}, {"state", std::string("great")}}, ClientRole::Service)
              .code == ErrorCode::BadRequest);
    CHECK(call(s, "/video/source/report", {{"source", std::int64_t{4}}, {"state", std::string("ok")}}, ClientRole::Service)
              .code == ErrorCode::NotFound);

    const Args two = {{"kind", std::string("display")},
                      {"1/id", std::string("A")},
                      {"1/name", std::string("Left")},
                      {"1/width", std::int64_t{1920}},
                      {"1/primary", true},
                      {"2/id", std::string("B")},
                      {"2/name", std::string("Right")}};
    REQUIRE(call(s, "/video/devices/report", two, ClientRole::Service).code == ErrorCode::Ok);
    CHECK(s.tree().getAs<std::string>("/video/devices/display/2/name") == "Right");
    // a shorter list replaces the old one completely
    REQUIRE(call(s, "/video/devices/report", {{"kind", std::string("display")}, {"1/id", std::string("B")}}, ClientRole::Service)
                .code == ErrorCode::Ok);
    CHECK(s.tree().getAs<std::string>("/video/devices/display/1/id") == "B");
    CHECK_FALSE(s.tree().contains("/video/devices/display/1/name"));
    CHECK_FALSE(s.tree().contains("/video/devices/display/2/id"));

    CHECK(call(s, "/video/devices/report", {{"kind", std::string("display")}, {"1/colour", std::string("x")}}, ClientRole::Service)
              .code == ErrorCode::BadRequest);
    CHECK(call(s, "/video/devices/report", {{"kind", std::string("display")}, {"1/width", std::string("wide")}}, ClientRole::Service)
              .code == ErrorCode::BadRequest);
    CHECK(call(s, "/video/devices/report", {{"kind", std::string("mice")}}, ClientRole::Service).code == ErrorCode::BadRequest);

    auto refresh = videoCall(s.tree(), "/video/devices/refresh", {}, ClientRole::Local);
    CHECK(refresh.result.code == ErrorCode::Ok);
    CHECK(refresh.event.name == "video.devices.refresh");
}

TEST_CASE("every video method is listed in the schema", "[video][schema]") {
    for (const auto& m : videoMethodPatterns()) CHECK(schemaV1().findMethod(m) != nullptr);
}
