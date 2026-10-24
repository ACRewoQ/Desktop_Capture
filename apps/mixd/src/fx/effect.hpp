#pragma once

// Insert effects of a channel strip. An Effect is created and configured off the audio thread and
// processed on it; parameter values cross over through atomics, so changing a knob never rebuilds
// the mix plan. FxSlot wraps an effect with the bypass crossfade and is what a StripPlan holds.

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dc::mixd {

inline constexpr int kFxMaxFrames = 1024; // = kMaxBlockFrames

class Effect {
public:
    virtual ~Effect() = default;

    // Any non-audio thread. key is the tree key (built-in: catalog key, VST3: "p<ParamID>"), value in
    // the parameter's units (VST3: normalized). Unknown keys are ignored.
    virtual void setParam(std::string_view key, double value) = 0;

    // Audio thread. In place on channels (1 or 2) x frames (<= kFxMaxFrames).
    virtual void process(float* const* x, int channels, int frames) = 0;

    // Audio thread: forget tails and history (called when the effect comes back from bypass).
    virtual void reset() = 0;

    virtual int latencySamples() const { return 0; }
};

// One insert position. Bypass switches with a 10 ms crossfade; a fully bypassed effect is not
// processed (no CPU, tails stop) and is reset before it comes back.
class FxSlot {
public:
    explicit FxSlot(std::shared_ptr<Effect> effect, bool bypass = false);

    Effect* effect() const { return effect_.get(); }
    const std::shared_ptr<Effect>& shared() const { return effect_; }

    // Any thread.
    void setBypass(bool b) { bypass_.store(b, std::memory_order_relaxed); }
    bool bypass() const { return bypass_.load(std::memory_order_relaxed); }

    // Audio thread.
    void process(float* const* x, int channels, int frames);

private:
    std::shared_ptr<Effect> effect_;
    std::atomic<bool> bypass_;
    float wet_; // audio thread: 0 = bypassed, 1 = active
    std::array<std::array<float, kFxMaxFrames>, 2> dry_{};
};

} // namespace dc::mixd
