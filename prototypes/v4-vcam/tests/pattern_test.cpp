// Round-trip check for the stamp: draw at 1080p, convert to NV12 (also scaled to 720p / 480p),
// decode from the luma plane. Runs anywhere (no Windows API).
#include <cstdio>
#include <vector>

#include "../common/pattern.hpp"

using namespace dc::vcam;

static int check(int sw, int sh, int dw, int dh, const Stamp& s) {
    std::vector<uint8_t> bgra(static_cast<size_t>(sw) * sh * 4);
    draw_test_pattern_bgra(bgra.data(), sw, sh, sw * 4, s);
    std::vector<uint8_t> y(static_cast<size_t>(dw) * dh), uv(static_cast<size_t>(dw) * dh / 2);
    bgra_to_nv12(bgra.data(), sw, sh, sw * 4, y.data(), dw, uv.data(), dw, dw, dh);
    auto got = decode_stamp(dw, dh, [&](int x, int yy) { return int(y[static_cast<size_t>(yy) * dw + x]); });
    const bool ok = got && got->qpc == s.qpc && got->frame_index == s.frame_index;
    std::printf("%s %dx%d -> NV12 %dx%d\n", ok ? "OK  " : "FAIL", sw, sh, dw, dh);
    return ok ? 0 : 1;
}

int main() {
    const Stamp s{0x0123456789ABCDEFll, 0xDEADBEEFu};
    int fails = 0;
    fails += check(1920, 1080, 1920, 1080, s);
    fails += check(1920, 1080, 1280, 720, s);
    fails += check(1920, 1080, 640, 480, s);
    fails += check(1280, 720, 1920, 1080, s);
    // no-signal picture must not decode
    std::vector<uint8_t> ns(1920 * 1080 * 4);
    draw_no_signal_bgra(ns.data(), 1920, 1080, 1920 * 4, 7);
    const bool none = !decode_stamp(1920, 1080, [&](int x, int yy) { return int(ns[(static_cast<size_t>(yy) * 1920 + x) * 4 + 1]); });
    std::printf("%s no-signal has no stamp\n", none ? "OK  " : "FAIL");
    fails += none ? 0 : 1;
    return fails;
}
