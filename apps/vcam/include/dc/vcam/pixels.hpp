// Pixel formats of the virtual camera: the "no signal" picture and the conversions the camera source and the
// producer need (nearest-neighbour scaling, BT.709 limited range). Portable, so it is unit-tested on Linux too.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dc::vcam {

// "No signal" picture shown by the camera when no producer is attached: dark diagonal stripes
// that drift, so a frozen pipeline is still visible. Carries no stamp.
inline void draw_no_signal_bgra(uint8_t* bgra, int width, int height, int pitch, uint64_t tick) {
    for (int y = 0; y < height; ++y) {
        uint8_t* row = bgra + static_cast<ptrdiff_t>(y) * pitch;
        for (int x = 0; x < width; ++x) {
            const bool stripe = ((x + y + static_cast<int>(tick * 2)) / 48) & 1;
            const uint8_t v = stripe ? 48 : 32;
            row[x * 4 + 0] = static_cast<uint8_t>(v + 16);
            row[x * 4 + 1] = v;
            row[x * 4 + 2] = v;
            row[x * 4 + 3] = 255;
        }
    }
}

// BGRA (any size) -> NV12 (dst size), nearest-neighbour scaling, BT.709 limited range.
inline void bgra_to_nv12(const uint8_t* src, int sw, int sh, int spitch, uint8_t* dy, int ypitch, uint8_t* duv,
                         int uvpitch, int dw, int dh) {
    auto px = [&](int x, int y) {
        const int sx = static_cast<int>(static_cast<int64_t>(x) * sw / dw);
        const int sy = static_cast<int>(static_cast<int64_t>(y) * sh / dh);
        return src + static_cast<ptrdiff_t>(sy) * spitch + sx * 4;
    };
    for (int y = 0; y < dh; ++y) {
        uint8_t* yrow = dy + static_cast<ptrdiff_t>(y) * ypitch;
        for (int x = 0; x < dw; ++x) {
            const uint8_t* p = px(x, y);
            // Y = 16 + 0.1826 R + 0.6142 G + 0.0620 B  (fixed point / 1024)
            yrow[x] = static_cast<uint8_t>(16 + ((187 * p[2] + 629 * p[1] + 63 * p[0]) >> 10));
        }
    }
    for (int y = 0; y < dh / 2; ++y) {
        uint8_t* uvrow = duv + static_cast<ptrdiff_t>(y) * uvpitch;
        for (int x = 0; x < dw / 2; ++x) {
            int r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; ++k) {
                const uint8_t* p = px(x * 2 + (k & 1), y * 2 + (k >> 1));
                b += p[0];
                g += p[1];
                r += p[2];
            }
            r /= 4, g /= 4, b /= 4;
            // Cb = 128 - 0.1006 R - 0.3386 G + 0.4392 B ; Cr = 128 + 0.4392 R - 0.3989 G - 0.0403 B
            uvrow[x * 2 + 0] = static_cast<uint8_t>(std::clamp(128 + ((-103 * r - 347 * g + 450 * b) >> 10), 0, 255));
            uvrow[x * 2 + 1] = static_cast<uint8_t>(std::clamp(128 + ((450 * r - 409 * g - 41 * b) >> 10), 0, 255));
        }
    }
}

// BGRA (any size) -> BGRA (dst size), nearest-neighbour scaling.
inline void bgra_scale(const uint8_t* src, int sw, int sh, int spitch, uint8_t* dst, int dpitch, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        const uint8_t* srow = src + static_cast<ptrdiff_t>(static_cast<int64_t>(y) * sh / dh) * spitch;
        uint8_t* drow = dst + static_cast<ptrdiff_t>(y) * dpitch;
        if (sw == dw) {
            std::memcpy(drow, srow, static_cast<size_t>(dw) * 4);
            continue;
        }
        for (int x = 0; x < dw; ++x)
            std::memcpy(drow + x * 4, srow + static_cast<int64_t>(x) * sw / dw * 4, 4);
    }
}

// NV12 (any size) -> NV12 (dst size), nearest-neighbour; a plain copy when the sizes match.
inline void nv12_scale(const uint8_t* sy, int sypitch, const uint8_t* suv, int suvpitch, int sw, int sh, uint8_t* dy,
                       int dypitch, uint8_t* duv, int duvpitch, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        const uint8_t* srow = sy + static_cast<ptrdiff_t>(static_cast<int64_t>(y) * sh / dh) * sypitch;
        uint8_t* drow = dy + static_cast<ptrdiff_t>(y) * dypitch;
        if (sw == dw) {
            std::memcpy(drow, srow, static_cast<size_t>(dw));
            continue;
        }
        for (int x = 0; x < dw; ++x) drow[x] = srow[static_cast<int64_t>(x) * sw / dw];
    }
    for (int y = 0; y < dh / 2; ++y) {
        const uint8_t* srow = suv + static_cast<ptrdiff_t>(static_cast<int64_t>(y) * (sh / 2) / (dh / 2)) * suvpitch;
        uint8_t* drow = duv + static_cast<ptrdiff_t>(y) * duvpitch;
        if (sw == dw) {
            std::memcpy(drow, srow, static_cast<size_t>(dw));
            continue;
        }
        for (int x = 0; x < dw / 2; ++x) {
            const int64_t sx = static_cast<int64_t>(x) * (sw / 2) / (dw / 2);
            drow[x * 2] = srow[sx * 2];
            drow[x * 2 + 1] = srow[sx * 2 + 1];
        }
    }
}

// NV12 (any size) -> BGRA (dst size), nearest-neighbour, BT.709 limited range.
inline void nv12_to_bgra(const uint8_t* sy, int sypitch, const uint8_t* suv, int suvpitch, int sw, int sh,
                         uint8_t* dst, int dpitch, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        const int syy = static_cast<int>(static_cast<int64_t>(y) * sh / dh);
        const uint8_t* yrow = sy + static_cast<ptrdiff_t>(syy) * sypitch;
        const uint8_t* uvrow = suv + static_cast<ptrdiff_t>(syy / 2) * suvpitch;
        uint8_t* drow = dst + static_cast<ptrdiff_t>(y) * dpitch;
        for (int x = 0; x < dw; ++x) {
            const int sx = static_cast<int>(static_cast<int64_t>(x) * sw / dw);
            const int c = yrow[sx] - 16;
            const int d = uvrow[(sx / 2) * 2] - 128;
            const int e = uvrow[(sx / 2) * 2 + 1] - 128;
            // R = 1.1644 Y' + 1.7927 Cr ; G = 1.1644 Y' - 0.2132 Cb - 0.5329 Cr ; B = 1.1644 Y' + 2.1124 Cb  (/1024)
            drow[x * 4 + 0] = static_cast<uint8_t>(std::clamp((1192 * c + 2163 * d + 512) >> 10, 0, 255));
            drow[x * 4 + 1] = static_cast<uint8_t>(std::clamp((1192 * c - 218 * d - 546 * e + 512) >> 10, 0, 255));
            drow[x * 4 + 2] = static_cast<uint8_t>(std::clamp((1192 * c + 1836 * e + 512) >> 10, 0, 255));
            drow[x * 4 + 3] = 255;
        }
    }
}

// BGRA -> packed BGR24 (same size, top-down, no row padding), as the DirectShow camera's sender takes it.
inline void bgra_to_bgr24(const uint8_t* src, int spitch, int w, int h, uint8_t* dst) {
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + static_cast<ptrdiff_t>(y) * spitch;
        uint8_t* d = dst + static_cast<ptrdiff_t>(y) * w * 3;
        for (int x = 0; x < w; ++x) {
            d[x * 3 + 0] = s[x * 4 + 0];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 2];
        }
    }
}

}  // namespace dc::vcam
