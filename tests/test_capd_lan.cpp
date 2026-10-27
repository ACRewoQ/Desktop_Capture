#include "lan_stream.hpp"

#include "dc/lanmedia/video_link.hpp"
#include "dc/protocol/defaults.hpp"
#include "dc/protocol/param_store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <mutex>
#include <thread>

using namespace dc;
using namespace dc::capd;

namespace {

proto::ParamStore seeded() {
    proto::ParamStore s;
    s.publish(proto::globalDefaults());
    return s;
}

using Bytes = std::vector<std::uint8_t>;

Bytes nal(int type, std::size_t payload, bool longStart = true) {
    Bytes b = longStart ? Bytes{0, 0, 0, 1} : Bytes{0, 0, 1};
    b.push_back(static_cast<std::uint8_t>(0x60 | type));
    for (std::size_t i = 0; i < payload; ++i) b.push_back(static_cast<std::uint8_t>(0x80 + type));
    return b;
}

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

Bytes bytes(std::span<const std::uint8_t> s) { return Bytes(s.begin(), s.end()); }

}  // namespace

TEST_CASE("the LAN video streams' settings are read from the tree", "[capd][lan]") {
    auto s = seeded();
    auto p = lanStreamConfig(s.tree(), "preview");
    CHECK(p.stream == "video/preview");
    CHECK(p.mix == 1);
    CHECK(p.width == 960);
    CHECK(p.height == 540);
    CHECK(p.fps == 30);
    CHECK(p.bitrateKbps == 3000);
    auto l = lanStreamConfig(s.tree(), "lan");
    CHECK(l.stream == "video/lan");
    CHECK(l.mix == 0);  // off until a mix is chosen
    CHECK(l.width == 1920);
    CHECK(l.height == 1080);
    CHECK(l.bitrateKbps == 8000);

    s.publish({{"/video/preview/width", std::int64_t{641}},
               {"/video/preview/height", std::int64_t{361}},
               {"/video/preview/fps", std::int64_t{60}},
               {"/video/lan/mix", std::int64_t{2}}});
    p = lanStreamConfig(s.tree(), "preview");
    CHECK(p.width == 640);  // NV12 needs even sizes
    CHECK(p.height == 360);
    CHECK(p.fps == 60);
    CHECK(lanStreamConfig(s.tree(), "lan").mix == 2);
    // the scene page's stream: the preview's settings, the mix's next scene
    const auto n = lanStreamConfig(s.tree(), "next");
    CHECK(n.stream == "video/next");
    CHECK(n.next);
    CHECK_FALSE(p.next);
    CHECK(n.mix == p.mix);
    CHECK(n.width == 640);
    CHECK(n.height == 360);
    CHECK(n.fps == 60);
    CHECK(n.bitrateKbps == p.bitrateKbps);

    const auto v = lanVideoSettings(p);
    CHECK(v.codec == "h264");
    CHECK(v.rateControl == "cbr");
    CHECK(v.lowLatency);
    CHECK(v.bitrateKbps == 3000);
    CHECK(v.width == 640);
    CHECK(v.fps == 60);
}

TEST_CASE("the LAN video streams go to the engine's subscribers of that stream", "[capd][lan]") {
    auto s = seeded();
    s.publish({{"/lan/sub/1/stream", std::string("video/preview")},
               {"/lan/sub/1/address", std::string("192.168.0.68:50123")},
               {"/lan/sub/2/stream", std::string("audio/1")},
               {"/lan/sub/2/address", std::string("192.168.0.68:50124")},
               {"/lan/sub/3/stream", std::string("video/preview")},
               {"/lan/sub/3/address", std::string("not-an-address:5000")},
               {"/lan/sub/4/stream", std::string("video/lan")},
               {"/lan/sub/4/address", std::string("10.0.0.2:6000")}});
    const auto preview = lanDestinations(s.tree(), "video/preview");
    REQUIRE(preview.size() == 1);
    CHECK(preview[0].address().to_string() == "192.168.0.68");
    CHECK(preview[0].port() == 50123);
    const auto lan = lanDestinations(s.tree(), "video/lan");
    REQUIRE(lan.size() == 1);
    CHECK(lan[0].port() == 6000);
}

TEST_CASE("every keyframe sent over the LAN carries SPS and PPS", "[capd][lan]") {
    ParameterSets ps;
    const Bytes sps = nal(7, 10), pps = nal(8, 4, false), idr = nal(5, 100), p = nal(1, 50), aud = nal(9, 1);

    // nothing to add before the encoder has shown its parameter sets
    const Bytes bare = cat({idr});
    CHECK(bytes(ps.process(bare, true)) == bare);
    CHECK_FALSE(ps.haveSets());

    const Bytes first = cat({aud, sps, pps, idr});
    CHECK(bytes(ps.process(first, true)) == first);  // already complete
    CHECK(ps.haveSets());
    CHECK(bytes(ps.process(cat({aud, p}), false)) == cat({aud, p}));
    // a keyframe without them gets the last ones, after the access unit delimiter
    CHECK(bytes(ps.process(cat({aud, idr}), true)) == cat({aud, sps, pps, idr}));
    CHECK(bytes(ps.process(cat({idr}), true)) == cat({sps, pps, idr}));
    CHECK(ps.added() == 2);

    // new parameter sets replace the old ones
    const Bytes sps2 = nal(7, 12), pps2 = nal(8, 6);
    ps.process(cat({sps2, pps2, idr}), true);
    CHECK(bytes(ps.process(cat({idr}), true)) == cat({sps2, pps2, idr}));
}

TEST_CASE("a LAN video stream reaches a subscribed viewer, keyframes decodable on their own", "[capd][lan][udp]") {
    lan::VideoOutput::Options oo;
    oo.bindAddress = "127.0.0.1";
    oo.port = 0;
    lan::VideoOutput out(oo);

    std::mutex m;
    std::vector<std::pair<bool, Bytes>> got;
    lan::VideoInput::Options io;
    io.bindAddress = "127.0.0.1";
    lan::VideoInput in(io, [&](const lan::VideoReceiver::Frame& f) {
        std::lock_guard lock(m);
        got.emplace_back(f.keyframe, Bytes(f.data.begin(), f.data.end()));
    });

    auto s = seeded();
    s.publish({{"/lan/sub/1/stream", std::string("video/preview")},
               {"/lan/sub/1/address", "127.0.0.1:" + std::to_string(in.localPort())}});
    out.setDestinations(lanDestinations(s.tree(), "video/preview"));
    REQUIRE(out.hasDestinations());

    // like the encoder's sink: SPS/PPS only on the first IDR, later keyframes without
    ParameterSets ps;
    const Bytes sps = nal(7, 10), pps = nal(8, 4), idr = nal(5, 5000), p = nal(1, 2000);
    for (int f = 0; f < 40; ++f) {
        const bool key = f % 20 == 0;
        const Bytes au = key ? (f == 0 ? cat({sps, pps, idr}) : idr) : p;
        out.send(ps.process(au, key), key, lan::nowSeconds());
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::lock_guard lock(m);
    REQUIRE(got.size() == 40);
    for (std::size_t i = 0; i < got.size(); ++i) {
        INFO("frame " << i);
        CHECK(got[i].first == (i % 20 == 0));
        if (got[i].first) CHECK(got[i].second == cat({sps, pps, idr}));
    }
}
