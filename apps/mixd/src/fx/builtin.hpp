#pragma once

// The built-in effects (docs/spec/protocol.md 5.7): eq4, comp, gate, reverb, delay, limiter. Their
// parameters are the catalog in dc/protocol/fx.hpp. All processing is 48 kHz, 1 or 2 channels,
// real-time safe (buffers are allocated in the constructors).

#include "effect.hpp"

#include "dc/protocol/fx.hpp"

#include <memory>
#include <vector>

namespace dc::mixd {

// Base for built-in effects: one atomic per catalog parameter, initialised to the defaults.
class BuiltinEffect : public Effect {
public:
    explicit BuiltinEffect(const proto::FxTypeSpec& spec);

    void setParam(std::string_view key, double value) override;
    int latencySamples() const override { return spec_.latencySamples; }

    const proto::FxTypeSpec& spec() const { return spec_; }
    // Audio thread: current target value of parameter i (catalog order), clamped to its range.
    float param(std::size_t i) const { return values_[i].load(std::memory_order_relaxed); }

    // Gain reduction in dB (<= 0) of the dynamics effects, for meters; 0 for the others.
    virtual float gainReductionDb() const { return 0; }

protected:
    // Audio thread: true once after any parameter changed (cheap check before recomputing).
    bool paramsChanged() { return changed_.exchange(false, std::memory_order_acquire); }

private:
    const proto::FxTypeSpec& spec_;
    std::unique_ptr<std::atomic<float>[]> values_;
    std::atomic<bool> changed_{true};
};

// nullptr for an unknown type.
std::shared_ptr<BuiltinEffect> makeBuiltinEffect(std::string_view type);

} // namespace dc::mixd
