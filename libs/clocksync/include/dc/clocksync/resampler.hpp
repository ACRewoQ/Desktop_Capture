#pragma once

#include <cstddef>
#include <vector>

namespace dc::clocksync {

// Windowed-sinc (Kaiser) interpolator with a continuously variable ratio, for ratios near 1.
// It corrects small clock differences between devices, so it does not lower the cutoff for
// downsampling. Interleaved float frames, any channel count. Real-time safe after construction.
//
//   ratio = input frames consumed per output frame (> 1 consumes faster than it produces)
//
// Usage per block:
//   n = r.inputFramesRequired(outFrames, ratio);
//   copy n frames to r.inputWritePointer(n), then r.commitInput(n);
//   r.produce(out, outFrames, ratio);
class VariableResampler {
public:
    struct Quality {
        int halfTaps = 32;      // zero crossings on each side of the kernel (taps = 2 * halfTaps)
        int phases = 512;       // polyphase table resolution (linear interpolation between phases)
        double cutoff = 0.90;   // relative to Nyquist (0.90 at 48 kHz = 21.6 kHz)
        double kaiserBeta = 10; // stopband about -100 dB
    };

    explicit VariableResampler(int channels) : VariableResampler(channels, Quality{}) {}
    VariableResampler(int channels, Quality quality, std::size_t maxBlockFrames = 8192);

    // Input frames that still have to be committed before produce(outFrames, ratio) can run.
    std::size_t inputFramesRequired(std::size_t outFrames, double ratio) const;

    // Room for `frames` more input frames (interleaved). Valid until the next produce/reset.
    float* inputWritePointer(std::size_t frames);
    void commitInput(std::size_t frames);

    // Requires inputFramesRequired(outFrames, ratio) == 0.
    void produce(float* out, std::size_t outFrames, double ratio);

    // Input frames committed but not yet passed by the read position (fractional).
    // The read position trails the newest committed input by this much.
    double bufferedFrames() const { return double(end_) - pos_; }

    // Clears history and restarts with halfTaps frames of silence in front of the next input.
    void reset();

    int channels() const { return channels_; }
    int halfTaps() const { return q_.halfTaps; }

private:
    void compact();

    int channels_;
    Quality q_;
    std::vector<float> table_; // (phases + 1) rows of 2 * halfTaps weights
    std::vector<float> buf_;   // interleaved input history
    std::size_t capacity_;     // frames in buf_
    std::size_t end_ = 0;      // frames committed into buf_
    double pos_ = 0;           // position of the next output frame, in buf_ frames
};

} // namespace dc::clocksync
