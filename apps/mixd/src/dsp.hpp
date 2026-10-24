#pragma once

// Small DSP parts of a channel strip: gain/pan smoothing and the Lo-Hi filters (2nd order Butterworth
// high-pass "low cut" and low-pass "high cut"). Real-time safe, no allocation.

#include <cmath>
#include <cstddef>

namespace dc::mixd {

inline constexpr double kSampleRate = 48000;
inline constexpr double kPi = 3.14159265358979323846;

// -144 dB and below is silence.
inline float dbToLinear(double db) { return db <= -144.0 ? 0.0f : static_cast<float>(std::pow(10.0, db / 20.0)); }

// One-pole smoothing towards a target, per sample. ~5 ms time constant removes zipper noise and
// clicks from fader moves, mutes and pan changes.
class Smoothed {
public:
    void reset(float v) { current_ = target_ = v; }
    void setTarget(float v) { target_ = v; }
    float target() const { return target_; }
    float current() const { return current_; }
    bool settled() const { return current_ == target_; }
    float next() {
        if (current_ != target_) {
            current_ += (target_ - current_) * kCoeff;
            if (std::fabs(target_ - current_) < 1e-6f) current_ = target_;
        }
        return current_;
    }

private:
    static constexpr float kCoeff = 0.0042f; // 1 - exp(-1 / (5 ms * 48 kHz))
    float current_ = 0;
    float target_ = 0;
};

// Pan law. Mono tracks use constant power (-3 dB in the centre); stereo tracks and the master use
// balance (the centre leaves both sides untouched).
struct PanGains {
    float left = 1;
    float right = 1;
};
PanGains panGains(float pan, bool stereoSource);

// Transposed direct form II biquad.
class Biquad {
public:
    enum class Type { HighPass, LowPass };
    void setup(Type type, double freqHz);
    void reset() { z1_ = z2_ = 0; }
    float process(float x) {
        const double y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return static_cast<float>(y);
    }

private:
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    double z1_ = 0, z2_ = 0;
};

// A switchable filter on up to two channels. Turning it on or off crossfades between the dry and the
// filtered signal over ~10 ms; frequency changes glide so a dragged knob does not crackle.
class StripFilter {
public:
    explicit StripFilter(Biquad::Type type) : type_(type) {}
    void reset(bool enabled, double freqHz);
    // Applies to frames samples of each channel in place. Updates the coefficients once per call.
    void process(float* const* channels, int channelCount, int frames, bool enabled, double freqHz);

private:
    Biquad::Type type_;
    Biquad biquad_[2];
    double freq_ = 1000;
    float wet_ = 0;
};

} // namespace dc::mixd
