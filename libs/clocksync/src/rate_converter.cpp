#include "dc/clocksync/rate_converter.hpp"

#include <algorithm>
#include <cmath>

namespace dc::clocksync {

VariableResampler::Quality RateConverter::qualityFor(double inRate, double outRate) {
    VariableResampler::Quality q;
    // the kernel's cutoff is relative to the input Nyquist: going down in rate it has to sit below
    // the output Nyquist instead, or everything above it folds back (48 -> 44.1 kHz: 19.8 kHz)
    if (outRate < inRate) q.cutoff *= outRate / inRate;
    return q;
}

RateConverter::RateConverter(int channels, double inRate, double outRate, std::size_t maxBlockFrames)
    : channels_(channels), inRate_(inRate), outRate_(outRate), ratio_(inRate / outRate),
      maxBlock_(std::max<std::size_t>(maxBlockFrames, 16)),
      // pull() commits up to maxBlock * ratio input frames at once
      r_(channels, qualityFor(inRate, outRate),
         static_cast<std::size_t>(std::ceil(static_cast<double>(maxBlock_) * std::max(1.0, ratio_))) + 8) {
    const auto room = static_cast<double>(maxBlock_ + 4 * static_cast<std::size_t>(r_.halfTaps()) + 16) / ratio_;
    out_.assign((static_cast<std::size_t>(room) + 16) * static_cast<std::size_t>(channels_), 0.0f);
}

std::size_t RateConverter::available() const {
    // largest n with inputFramesRequired(n) == 0: the last output frame's kernel must end inside
    // the committed input
    const double usable = r_.bufferedFrames() - r_.halfTaps() - 1;
    if (usable < 0) return 0;
    auto n = static_cast<std::size_t>(std::floor(usable / ratio_)) + 1;
    while (n > 0 && r_.inputFramesRequired(n, ratio_) > 0) --n;
    return std::min(n, out_.size() / static_cast<std::size_t>(channels_));
}

} // namespace dc::clocksync
