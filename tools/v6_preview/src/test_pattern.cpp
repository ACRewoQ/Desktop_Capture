#include "test_pattern.hpp"

#include <algorithm>
#include <cstring>

namespace v6 {

TestPattern::TestPattern(int width, int height)
    : w_(width & ~1), h_(height & ~1), buf_(static_cast<size_t>(w_) * h_ * 3 / 2) {}

void TestPattern::fillRect(int x, int y, int w, int h, std::uint8_t luma) {
    const int x0 = std::clamp(x, 0, w_), x1 = std::clamp(x + w, 0, w_);
    const int y0 = std::clamp(y, 0, h_), y1 = std::clamp(y + h, 0, h_);
    for (int yy = y0; yy < y1; ++yy) std::memset(&buf_[static_cast<size_t>(yy) * w_ + x0], luma, x1 - x0);
}

// Seven-segment digit; segment thickness = scale, digit cell = 6*scale x 10*scale.
void TestPattern::drawDigit(int x, int y, int scale, int digit) {
    static const std::uint8_t kSegs[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    const int s = scale, L = 4 * scale;
    const std::uint8_t m = kSegs[digit % 10];
    const std::uint8_t on = 235;
    if (m & 0x01) fillRect(x + s, y, L, s, on);                  // a
    if (m & 0x02) fillRect(x + s + L, y + s, s, L, on);          // b
    if (m & 0x04) fillRect(x + s + L, y + 2 * s + L, s, L, on);  // c
    if (m & 0x08) fillRect(x + s, y + 2 * s + 2 * L, L, s, on);  // d
    if (m & 0x10) fillRect(x, y + 2 * s + L, s, L, on);          // e
    if (m & 0x20) fillRect(x, y + s, s, L, on);                  // f
    if (m & 0x40) fillRect(x + s, y + s + L, L, s, on);          // g
}

void TestPattern::render(std::uint64_t frameId, std::int64_t genTimeUs) {
    std::uint8_t* Y = buf_.data();
    std::uint8_t* UV = Y + static_cast<size_t>(w_) * h_;
    const int f = static_cast<int>(frameId);

    // Scrolling diagonal gradient.
    for (int y = 0; y < h_; ++y) {
        std::uint8_t* row = Y + static_cast<size_t>(y) * w_;
        const int base = y + f * 4;
        for (int x = 0; x < w_; ++x) row[x] = static_cast<std::uint8_t>(32 + ((x + base) & 127));
    }
    // Slowly cycling colour bands.
    for (int y = 0; y < h_ / 2; ++y) {
        std::uint8_t* row = UV + static_cast<size_t>(y) * w_;
        for (int x = 0; x < w_ / 2; ++x) {
            const int band = ((x * 8) / (w_ / 2) + f / 30) & 7;
            row[2 * x] = static_cast<std::uint8_t>(96 + band * 8);
            row[2 * x + 1] = static_cast<std::uint8_t>(160 - band * 8);
        }
    }
    // Noise blocks in the lower third (changes every frame, like moving desktop content).
    const int nb = 16;
    for (int by = h_ * 2 / 3; by + nb <= h_; by += nb) {
        for (int bx = 0; bx + nb <= w_; bx += nb) {
            rng_ = rng_ * 1664525u + 1013904223u;
            if ((rng_ >> 28) < 3) fillRect(bx, by, nb, nb, static_cast<std::uint8_t>(16 + ((rng_ >> 8) % 220)));
        }
    }
    // Bouncing box.
    const int boxW = w_ / 10, travel = w_ - boxW;
    int pos = (f * 12) % (2 * travel);
    if (pos > travel) pos = 2 * travel - pos;
    fillRect(pos, h_ / 2 - boxW / 2, boxW, boxW, 235);

    // Header strip: barcode (24 bits) + generation time digits.
    const int cell = std::max(8, w_ / 120);
    fillRect(0, 0, w_, cell * 14, 16);
    for (int bit = 0; bit < 24; ++bit) {
        const bool one = (frameId >> (23 - bit)) & 1;
        fillRect(cell + bit * cell, cell, cell, cell * 4, one ? 235 : 16);
    }
    fillRect(cell + 24 * cell, cell, cell / 2, cell * 4, 128);  // end marker

    const std::int64_t ms = (genTimeUs / 1000) % 1'000'000;
    const int scale = std::max(2, cell / 4);
    int x = cell * 28;
    for (int i = 5; i >= 0; --i) {
        std::int64_t p = 1;
        for (int k = 0; k < i; ++k) p *= 10;
        drawDigit(x, cell, scale, static_cast<int>((ms / p) % 10));
        x += 7 * scale;
    }
}

}  // namespace v6
