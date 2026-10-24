#pragma once

#include "dc/clocksync/resampler.hpp"

#include <cstddef>
#include <vector>

namespace dc::clocksync {

// Fixed-ratio sample rate conversion between a device rate (44.1 kHz, 96 kHz, ...) and the engine's
// 48 kHz, so devices that do not run at 48 kHz can be opened exclusive or without Windows' converter.
// The clock difference that remains between the device and the engine is still the ClockBridge's job;
// this only takes away the nominal ratio. Interleaved float frames. Real-time safe after construction.
//
// Two ways to drive it, one per stream direction:
//   capture: push(device frames) -> sink(48 kHz frames) as soon as they can be produced
//   render:  pull(device frames)  <- source(48 kHz frames) asked for exactly as many as needed
class RateConverter {
public:
    // maxBlockFrames: the largest block passed to push()/pull() (larger ones are split).
    RateConverter(int channels, double inRate, double outRate, std::size_t maxBlockFrames = 4096);

    int channels() const { return channels_; }
    double inRate() const { return inRate_; }
    double outRate() const { return outRate_; }

    // Capture side. time = when the first of `frames` was sampled (seconds). sink(out, count, time of
    // the first output frame) is called zero or more times. Sink: void(const float*, std::size_t, double).
    template <typename Sink>
    void push(const float* in, std::size_t frames, double time, Sink&& sink) {
        while (frames > 0) {
            const std::size_t n = frames < maxBlock_ ? frames : maxBlock_;
            // the next output frame sits this far behind the newest committed input
            const double outTime = time - r_.bufferedFrames() / inRate_;
            float* dst = r_.inputWritePointer(n);
            for (std::size_t i = 0; i < n * static_cast<std::size_t>(channels_); ++i) dst[i] = in[i];
            r_.commitInput(n);
            const std::size_t produced = available();
            if (produced > 0) {
                r_.produce(out_.data(), produced, ratio_);
                sink(static_cast<const float*>(out_.data()), produced, outTime);
            }
            in += n * static_cast<std::size_t>(channels_);
            frames -= n;
            time += static_cast<double>(n) / inRate_;
        }
    }

    // Render side: fills `frames` output frames, asking source(dst, count) for the input it needs.
    // Source: void(float* dst, std::size_t count) writes count interleaved input frames.
    template <typename Source>
    void pull(float* out, std::size_t frames, Source&& source) {
        while (frames > 0) {
            const std::size_t n = frames < maxBlock_ ? frames : maxBlock_;
            const std::size_t need = r_.inputFramesRequired(n, ratio_);
            if (need > 0) {
                source(r_.inputWritePointer(need), need);
                r_.commitInput(need);
            }
            r_.produce(out, n, ratio_);
            out += n * static_cast<std::size_t>(channels_);
            frames -= n;
        }
    }

    // Output frames per input frame (outRate / inRate).
    double outPerIn() const { return 1.0 / ratio_; }

    // Delay through the converter in seconds (half the kernel), for timestamps.
    double delaySeconds() const { return r_.halfTaps() / inRate_; }

    void reset() { r_.reset(); }

private:
    std::size_t available() const;
    static VariableResampler::Quality qualityFor(double inRate, double outRate);

    int channels_;
    double inRate_;
    double outRate_;
    double ratio_; // input frames per output frame
    std::size_t maxBlock_;
    VariableResampler r_;
    std::vector<float> out_;
};

} // namespace dc::clocksync
