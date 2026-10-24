// Test pattern with a machine-readable stamp (from the V4 verification). Used by "capd vcam --test-pattern" and
// dc-vcam-probe to measure what a virtual camera delivers.
//
// The stamp is two rows of 80 cells in the top-left corner. Cells are width/240 x height/135
// (8 x 8 px at 1080p), so the stamp stays decodable after the camera app or the frame server
// rescales the picture, even to another aspect ratio.
//   row 0: 16 sync cells (1010...) + 64 bits of the producer's QueryPerformanceCounter
//   row 1: 32 bits frame index + 32 bits check (index ^ low32(qpc) ^ 0xA5A5A5A5) + 16 sync cells
// A probe that grabs frames from the camera decodes it to count frames, detect drops and measure
// latency (QPC is system wide, so producer and probe clocks agree).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>

namespace dc::vcam {

struct Stamp {
    int64_t qpc = 0;
    uint32_t frame_index = 0;
};

inline constexpr int kStampCells = 80;

inline double stamp_cell_w(int width) { return width / 240.0; }
inline double stamp_cell_h(int height) { return height / 135.0; }

inline uint32_t stamp_check(const Stamp& s) {
    return s.frame_index ^ static_cast<uint32_t>(static_cast<uint64_t>(s.qpc)) ^ 0xA5A5A5A5u;
}

inline bool stamp_bit(const Stamp& s, int row, int cell) {
    if (row == 0) {
        if (cell < 16) return (cell & 1) == 0;
        return ((static_cast<uint64_t>(s.qpc) >> (cell - 16)) & 1u) != 0;
    }
    if (cell < 32) return ((s.frame_index >> cell) & 1u) != 0;
    if (cell < 64) return ((stamp_check(s) >> (cell - 32)) & 1u) != 0;
    return (cell & 1) == 0;
}

// BGRA, top-down. `pitch` in bytes.
inline void draw_stamp_bgra(uint8_t* bgra, int width, int height, int pitch, const Stamp& s) {
    const double cw = stamp_cell_w(width), ch = stamp_cell_h(height);
    const int xend = std::min(width, static_cast<int>(kStampCells * cw));
    const int yend = std::min(height, static_cast<int>(2 * ch));
    for (int y = 0; y < yend; ++y) {
        uint8_t* p = bgra + static_cast<ptrdiff_t>(y) * pitch;
        const int row = std::min(1, static_cast<int>(y / ch));
        for (int x = 0; x < xend; ++x) {
            const uint8_t v = stamp_bit(s, row, std::min(kStampCells - 1, static_cast<int>(x / cw))) ? 255 : 0;
            p[x * 4 + 0] = v;
            p[x * 4 + 1] = v;
            p[x * 4 + 2] = v;
            p[x * 4 + 3] = 255;
        }
    }
}

// `luma(x, y)` returns 0..255 brightness at a pixel of a width x height picture (top-down).
template <class LumaFn>
std::optional<Stamp> decode_stamp(int width, int height, LumaFn&& luma) {
    const double cw = stamp_cell_w(width), ch = stamp_cell_h(height);
    if (cw < 2.0 || ch < 2.0) return std::nullopt;
    // Average a small window at the cell centre (1 px when cells are tiny).
    const int r = (cw >= 5.0 && ch >= 5.0) ? 1 : 0;
    auto bit = [&](int row, int c) {
        int sum = 0, n = 0;
        const int cx = static_cast<int>((c + 0.5) * cw), cy = static_cast<int>((row + 0.5) * ch);
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx, ++n)
                sum += luma(std::clamp(cx + dx, 0, width - 1), std::clamp(cy + dy, 0, height - 1));
        return sum / n >= 128;
    };
    for (int c = 0; c < 16; ++c) {
        if (bit(0, c) != ((c & 1) == 0)) return std::nullopt;
        if (bit(1, 64 + c) != (((64 + c) & 1) == 0)) return std::nullopt;
    }
    Stamp s;
    uint64_t q = 0;
    for (int c = 0; c < 64; ++c)
        if (bit(0, 16 + c)) q |= 1ull << c;
    s.qpc = static_cast<int64_t>(q);
    uint32_t idx = 0, chk = 0;
    for (int c = 0; c < 32; ++c) {
        if (bit(1, c)) idx |= 1u << c;
        if (bit(1, 32 + c)) chk |= 1u << c;
    }
    s.frame_index = idx;
    if (chk != stamp_check(s)) return std::nullopt;
    return s;
}

// Animated test picture: SMPTE-like colour bars, a moving white block and a frame-index bar graph,
// with the stamp on top. BGRA, top-down.
inline void draw_test_pattern_bgra(uint8_t* bgra, int width, int height, int pitch, const Stamp& s) {
    static constexpr uint8_t bars[7][3] = {  // B, G, R (75% bars)
        {191, 191, 191}, {0, 191, 191}, {191, 191, 0}, {0, 191, 0}, {191, 0, 191}, {0, 0, 191}, {191, 0, 0}};
    const int block = height / 6;
    const int span = std::max(1, width - block);
    const int phase = static_cast<int>((s.frame_index * 8u) % static_cast<uint32_t>(2 * span));
    const int bx = phase < span ? phase : 2 * span - phase;
    const int by = height / 2 - block / 2;
    for (int y = 0; y < height; ++y) {
        uint8_t* row = bgra + static_cast<ptrdiff_t>(y) * pitch;
        for (int x = 0; x < width; ++x) {
            const uint8_t* c = bars[std::min(6, x * 7 / width)];
            uint8_t b = c[0], g = c[1], r = c[2];
            if (y >= height * 3 / 4) {  // bottom quarter: grey ramp
                b = g = r = static_cast<uint8_t>(x * 255 / std::max(1, width - 1));
            }
            if (x >= bx && x < bx + block && y >= by && y < by + block) b = g = r = 255;
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
    draw_stamp_bgra(bgra, width, height, pitch, s);
}

}  // namespace dc::vcam
