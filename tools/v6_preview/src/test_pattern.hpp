// NV12 test frames that stand in for the captured desktop.
//
// Each frame carries, burned into the luma plane:
//   - the frame id as a 24-bit barcode (top-left), so a screenshot shows which frame is on screen
//   - the generation time (QPC milliseconds, last 6 digits) as seven-segment digits, so a screenshot
//     of the preview next to the on-screen clock gives an independent latency reading
//   - moving content and changing noise blocks so the encoder produces a realistic bitrate
#pragma once

#include <cstdint>
#include <vector>

namespace v6 {

class TestPattern {
public:
    TestPattern(int width, int height);

    int width() const { return w_; }
    int height() const { return h_; }
    // Y plane (width*height) followed by interleaved UV (width*height/2), tightly packed.
    const std::vector<std::uint8_t>& nv12() const { return buf_; }

    void render(std::uint64_t frameId, std::int64_t genTimeUs);

private:
    void fillRect(int x, int y, int w, int h, std::uint8_t luma);
    void drawDigit(int x, int y, int scale, int digit);

    int w_, h_;
    std::vector<std::uint8_t> buf_;
    std::uint32_t rng_ = 0x12345678u;
};

}  // namespace v6
