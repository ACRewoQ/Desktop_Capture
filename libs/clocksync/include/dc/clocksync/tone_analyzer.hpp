#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dc::clocksync {

// Checks a stream that should carry one continuous sine tone (mono, or channel 0 of a stream).
// Each block of blockFrames gets a four-parameter sine fit (amplitude, phase, frequency, offset,
// IEEE 1057); whatever the fit cannot explain is noise or a glitch. A dropped, repeated or
// discontinuous sample shows up as a block whose SINAD falls far below the stream's normal level.
//
// The analyzer also unwraps the tone phase from block to block. Divided by the tone's phase step per
// source frame it gives "which source frame is playing here" (up to a constant), so a caller that
// knows when each source frame was produced can measure end-to-end latency and its drift.
class ToneAnalyzer {
public:
    struct Config {
        double sampleRate = 48000;
        double toneHz = 1000;           // nominal tone frequency (the fit tracks small deviations)
        std::size_t blockFrames = 4800; // 100 ms
        double glitchSinadDb = 60;      // a block below this is a glitch
        double minAmplitude = 0.01;     // a block below this (peak) counts as a dropout
        std::size_t maxEvents = 50;     // glitch events kept for the report
    };

    struct Block {
        std::uint64_t index = 0;  // block number since the start
        std::uint64_t startFrame = 0;
        double amplitude = 0;     // peak
        double frequencyHz = 0;
        double sinadDb = 0;
        double unwrappedPhase = 0; // tone phase at startFrame, continuous over the stream (radians)
        bool glitch = false;
    };

    struct Summary {
        std::uint64_t blocks = 0;
        std::uint64_t glitchBlocks = 0;
        double minSinadDb = 1e9;
        double meanSinadDb = 0;
        std::vector<Block> events; // first maxEvents glitch blocks
    };

    ToneAnalyzer() : ToneAnalyzer(Config{}) {}
    explicit ToneAnalyzer(Config config);

    // Feeds interleaved samples; only channel 0 is analyzed. Calls onBlock for every finished block.
    template <typename F> void feed(const float* samples, std::size_t frames, int channels, F&& onBlock) {
        for (std::size_t i = 0; i < frames; ++i) {
            buf_.push_back(samples[i * channels]);
            if (buf_.size() == cfg_.blockFrames) onBlock(analyzeBlock());
        }
    }
    void feed(const float* samples, std::size_t frames, int channels) {
        feed(samples, frames, channels, [](const Block&) {});
    }

    // Starts counting glitches from now on (blocks before were warm-up). Keeps phase tracking.
    void resetSummary() { summary_ = Summary{}; sinadSum_ = 0; }
    // Forgets the phase history (after a known, accepted discontinuity).
    void resetTracking() { tracking_ = false; omega_ = nominalOmega_; }

    const Summary& summary() const { return summary_; }
    const Config& config() const { return cfg_; }
    // Tone phase advance per frame at the nominal frequency.
    double nominalOmega() const { return nominalOmega_; }

private:
    Block analyzeBlock();

    Config cfg_;
    double nominalOmega_;
    double omega_;
    bool tracking_ = false;
    double phase_ = 0;      // unwrapped phase at the start of the next block
    std::uint64_t next_ = 0;
    std::vector<float> buf_;
    Summary summary_;
    double sinadSum_ = 0;
};

} // namespace dc::clocksync
