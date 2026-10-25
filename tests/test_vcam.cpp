// Virtual camera pixel helpers (dc/vcam/pixels.hpp) and the test-pattern stamp: portable parts of apps/vcam.

#include "dc/vcam/pattern.hpp"
#include "dc/vcam/pixels.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace dc::vcam;

namespace {

struct Nv12 {
    int w, h;
    std::vector<std::uint8_t> data;
    Nv12(int width, int height) : w(width), h(height), data(std::size_t(width) * height * 3 / 2) {}
    std::uint8_t* y() { return data.data(); }
    std::uint8_t* uv() { return data.data() + std::size_t(w) * h; }
};

std::vector<std::uint8_t> bars(int w, int h) {
    // Eight vertical colour bars, flat inside (chroma subsampling then loses nothing away from the edges).
    static const std::uint8_t colours[8][3] = {{255, 255, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0},
                                               {255, 0, 255},   {0, 0, 255},   {255, 0, 0},   {16, 16, 16}};
    std::vector<std::uint8_t> px(std::size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const auto* c = colours[x * 8 / w];
            std::uint8_t* p = &px[(std::size_t(y) * w + x) * 4];
            p[0] = c[0];
            p[1] = c[1];
            p[2] = c[2];
            p[3] = 255;
        }
    return px;
}

}  // namespace

TEST_CASE("BGRA -> NV12 -> BGRA keeps the colours of flat areas", "[vcam]") {
    const int w = 64, h = 16;
    const auto src = bars(w, h);
    Nv12 nv(w, h);
    bgra_to_nv12(src.data(), w, h, w * 4, nv.y(), w, nv.uv(), w, w, h);
    // White is Y 235, black-ish 16 is Y 16+...: limited range.
    CHECK(std::abs(int(nv.y()[0]) - 235) <= 1);
    std::vector<std::uint8_t> back(std::size_t(w) * h * 4);
    nv12_to_bgra(nv.y(), w, nv.uv(), w, w, h, back.data(), w * 4, w, h);
    for (int bar = 0; bar < 8; ++bar) {
        const int x = bar * 8 + 4;  // middle of the bar
        for (int c = 0; c < 3; ++c) {
            const int a = src[(std::size_t(5) * w + x) * 4 + c], b = back[(std::size_t(5) * w + x) * 4 + c];
            INFO("bar " << bar << " channel " << c);
            CHECK(std::abs(a - b) <= 6);
        }
        CHECK(back[(std::size_t(5) * w + x) * 4 + 3] == 255);
    }
}

TEST_CASE("NV12 scaling copies at the same size and samples the nearest pixel otherwise", "[vcam]") {
    const int w = 32, h = 8;
    Nv12 a(w, h);
    for (std::size_t i = 0; i < a.data.size(); ++i) a.data[i] = std::uint8_t(i * 7);
    // Destination with a wider pitch than the width, as camera buffers have.
    const int pitch = 48;
    std::vector<std::uint8_t> dy(std::size_t(pitch) * h), duv(std::size_t(pitch) * h / 2);
    nv12_scale(a.y(), w, a.uv(), w, w, h, dy.data(), pitch, duv.data(), pitch, w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) REQUIRE(dy[std::size_t(y) * pitch + x] == a.y()[std::size_t(y) * w + x]);
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w; ++x) REQUIRE(duv[std::size_t(y) * pitch + x] == a.uv()[std::size_t(y) * w + x]);

    // Half size: every second pixel, and U/V stay paired.
    Nv12 half(w / 2, h / 2);
    nv12_scale(a.y(), w, a.uv(), w, w, h, half.y(), w / 2, half.uv(), w / 2, w / 2, h / 2);
    CHECK(half.y()[1] == a.y()[2]);
    CHECK(half.y()[std::size_t(w / 2) + 1] == a.y()[std::size_t(2) * w + 2]);
    CHECK(half.uv()[2] == a.uv()[4]);  // U of the second chroma pair = U of the source's third pair
    CHECK(half.uv()[3] == a.uv()[5]);
}

TEST_CASE("BGRA packs to BGR24 rows without padding", "[vcam]") {
    const int w = 3, h = 2, pitch = 16;
    std::vector<std::uint8_t> src(std::size_t(pitch) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c) src[std::size_t(y) * pitch + x * 4 + c] = std::uint8_t(y * 100 + x * 10 + c);
    std::vector<std::uint8_t> dst(std::size_t(w) * h * 3);
    bgra_to_bgr24(src.data(), pitch, w, h, dst.data());
    CHECK(dst[0] == 0);
    CHECK(dst[2] == 2);
    CHECK(dst[3] == 10);
    CHECK(dst[std::size_t(w) * 3] == 100);  // second row starts right after the first
    CHECK(dst[std::size_t(w) * 3 + 8] == 122);
}

TEST_CASE("the test-pattern stamp survives NV12 and a resize", "[vcam]") {
    const int w = 1920, h = 1080;
    std::vector<std::uint8_t> px(std::size_t(w) * h * 4);
    const Stamp s{123456789012345, 4242};
    draw_test_pattern_bgra(px.data(), w, h, w * 4, s);
    Nv12 nv(1280, 720);
    bgra_to_nv12(px.data(), w, h, w * 4, nv.y(), 1280, nv.uv(), 1280, 1280, 720);
    const auto got = decode_stamp(1280, 720, [&](int x, int y) { return int(nv.y()[std::size_t(y) * 1280 + x]); });
    REQUIRE(got);
    CHECK(got->qpc == s.qpc);
    CHECK(got->frame_index == s.frame_index);
}
