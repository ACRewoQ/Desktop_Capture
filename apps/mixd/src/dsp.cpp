#include "dsp.hpp"

#include <algorithm>

namespace dc::mixd {

PanGains panGains(float pan, bool stereoSource) {
    pan = std::clamp(pan, -1.0f, 1.0f);
    if (stereoSource) return {pan > 0 ? 1.0f - pan : 1.0f, pan < 0 ? 1.0f + pan : 1.0f};
    const double angle = (static_cast<double>(pan) + 1.0) * kPi / 4.0;
    return {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
}

void Biquad::setup(Type type, double freqHz) {
    // RBJ audio EQ cookbook, Q = 1/sqrt(2) (Butterworth)
    freqHz = std::clamp(freqHz, 10.0, kSampleRate * 0.49);
    const double w0 = 2 * kPi * freqHz / kSampleRate;
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2 * 0.7071067811865476);
    const double a0 = 1 + alpha;
    if (type == Type::HighPass) {
        b0_ = (1 + cosw) / 2 / a0;
        b1_ = -(1 + cosw) / a0;
        b2_ = b0_;
    } else {
        b0_ = (1 - cosw) / 2 / a0;
        b1_ = (1 - cosw) / a0;
        b2_ = b0_;
    }
    a1_ = -2 * cosw / a0;
    a2_ = (1 - alpha) / a0;
}

void StripFilter::reset(bool enabled, double freqHz) {
    freq_ = freqHz;
    wet_ = enabled ? 1.0f : 0.0f;
    for (auto& b : biquad_) {
        b.setup(type_, freq_);
        b.reset();
    }
}

void StripFilter::process(float* const* channels, int channelCount, int frames, bool enabled, double freqHz) {
    const float wetTarget = enabled ? 1.0f : 0.0f;
    if (wet_ == 0 && wetTarget == 0) {
        freq_ = freqHz; // jump: nothing audible depends on it
        return;
    }
    if (wet_ == 0) { // switching on: start from a clean state at the requested frequency
        freq_ = freqHz;
        for (auto& b : biquad_) b.reset();
    }
    // glide in the log domain, ~30 ms to settle at 5 ms blocks
    if (freq_ != freqHz) {
        const double ratio = freqHz / freq_;
        freq_ = std::fabs(ratio - 1.0) < 0.001 ? freqHz : freq_ * std::pow(ratio, 0.35);
    }
    for (int c = 0; c < std::min(channelCount, 2); ++c) biquad_[c].setup(type_, freq_);

    constexpr float kStep = 1.0f / 480.0f; // 10 ms crossfade
    for (int c = 0; c < std::min(channelCount, 2); ++c) {
        float wet = wet_;
        float* x = channels[c];
        for (int i = 0; i < frames; ++i) {
            const float y = biquad_[c].process(x[i]);
            if (wet != wetTarget) wet = wetTarget > wet ? std::min(wetTarget, wet + kStep) : std::max(wetTarget, wet - kStep);
            x[i] = wet == 1.0f ? y : x[i] + (y - x[i]) * wet;
        }
        if (c == std::min(channelCount, 2) - 1) wet_ = wet;
    }
    if (wet_ == 0)
        for (auto& b : biquad_) b.reset();
}

} // namespace dc::mixd
