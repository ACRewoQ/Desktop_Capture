// Per-frame stage timestamps and latency summaries.
//
// Stages (all QPC microseconds, same process):
//   Gen       test frame rendered (stands in for "captured")
//   Encoded   access unit left the encoder
//   Received  access unit handed to the decode thread (after transport)
//   Decoded   decoder released the frame
//   Converted NV12 -> BGRA done and published to the shared texture
//   Copied    display thread copied it into Qt's texture (inside updatePaintNode)
//   Presented the frame containing it was presented (QQuickWindow::frameSwapped)
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace v6 {

enum Stage : int { Gen, Encoded, Received, Decoded, Converted, Copied, Presented, kStageCount };
const char* stageName(int s);

// Durations of one completed frame in microseconds; -1 = not measured.
struct FrameTiming {
    std::uint64_t id = 0;
    std::array<std::int64_t, kStageCount> step{};  // step[s] = t[s] - t[s-1]; step[0] unused
    std::int64_t total = -1;                       // first known stage -> final stage
};

struct Percentiles {
    std::size_t n = 0;
    double p50 = 0, p95 = 0, p99 = 0, max = 0, mean = 0;  // milliseconds
};

class Stats {
public:
    // finalStage: the stage that completes a frame. warmupFrames: frames ignored at the start.
    Stats(Stage finalStage, std::uint64_t warmupFrames);

    void mark(std::uint64_t id, Stage s, std::int64_t tUs);
    void mark(std::uint64_t id, Stage s);  // now
    // For timings assembled elsewhere (network sender: remote durations come back in the ack).
    void addTiming(const FrameTiming& t);
    void countGenerated() { generated_.fetch_add(1, std::memory_order_relaxed); }
    std::int64_t stamp(std::uint64_t id, Stage s) const;

    std::uint64_t generated() const { return generated_.load(); }
    std::uint64_t completed() const { return completed_.load(); }

    Percentiles totalPercentiles(bool recentOnly) const;
    Percentiles stepPercentiles(int stage) const;

    // One-line live summary (last ~2 s) and the full report.
    std::string liveLine() const;
    std::string report(const std::string& title) const;
    bool writeCsv(const std::string& path) const;

private:
    static constexpr std::size_t kRing = 4096;
    struct Slot {
        std::atomic<std::uint64_t> id{~0ull};
        std::array<std::atomic<std::int64_t>, kStageCount> t{};
    };

    Stage final_;
    std::uint64_t warmup_;
    std::vector<Slot> ring_;
    std::atomic<std::uint64_t> generated_{0}, completed_{0};

    mutable std::mutex mu_;
    std::vector<FrameTiming> timings_;
};

}  // namespace v6
