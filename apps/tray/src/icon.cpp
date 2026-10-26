#include "icon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dc::tray {

namespace {

struct Rgb {
    std::uint8_t r, g, b;
};

Rgb knobColour(Health h) {
    switch (h) {
    case Health::Ok: return {0x3F, 0xB9, 0x50};
    case Health::Busy: return {0xE3, 0xA0, 0x08};
    case Health::Failed: return {0xF8, 0x51, 0x49};
    case Health::Stopped: return {0x8B, 0x94, 0x9E};
    }
    return {0xFF, 0xFF, 0xFF};
}

} // namespace

HICON makeTrayIcon(Health health, int size) {
    const int n = size;
    const double s = n;
    const Rgb background{0x24, 0x29, 0x33}, track{0xB0, 0xB8, 0xC4}, knob = knobColour(health);
    const double radius = s * 0.2;
    const double trackHalf = std::max(0.5, s / 32.0);
    const double faderX[3] = {s * 0.27, s * 0.5, s * 0.73};
    const double knobY[3] = {s * 0.62, s * 0.36, s * 0.52};
    const double knobHalfW = s * 0.11, knobHalfH = s * 0.075;

    // Which colour a point is (or transparent): the shapes from the back to the front.
    auto sample = [&](double x, double y, Rgb& out) -> bool {
        const double cx = std::min(std::max(x, radius), s - radius), cy = std::min(std::max(y, radius), s - radius);
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > radius * radius) return false;
        out = background;
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(x - faderX[i]) <= trackHalf && y >= s * 0.16 && y <= s * 0.84) out = track;
            if (std::fabs(x - faderX[i]) <= knobHalfW && std::fabs(y - knobY[i]) <= knobHalfH) out = knob;
        }
        return true;
    };

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = n;
    bi.bV5Height = -n;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP colour = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!colour) return nullptr;

    // 4 x 4 samples per pixel for smooth edges.
    auto* px = static_cast<std::uint32_t*>(bits);
    constexpr int kSub = 4;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < kSub; ++sy)
                for (int sx = 0; sx < kSub; ++sx) {
                    Rgb c{};
                    if (!sample(x + (sx + 0.5) / kSub, y + (sy + 0.5) / kSub, c)) continue;
                    r += c.r;
                    g += c.g;
                    b += c.b;
                    a += 1;
                }
            // Straight (not premultiplied) alpha, as in icon files: the colour of the covered samples.
            const auto ch = [](double v, double d) { return d > 0 ? static_cast<std::uint32_t>(std::lround(v / d)) : 0u; };
            px[y * n + x] = (ch(a * 255, kSub * kSub) << 24) | (ch(r, a) << 16) | (ch(g, a) << 8) | ch(b, a);
        }

    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = colour;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(colour);
    DeleteObject(mask);
    return icon;
}

} // namespace dc::tray
