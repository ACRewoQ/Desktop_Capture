#include "dc/lanmedia/arrival_clock.hpp"

#include <algorithm>
#include <cmath>

namespace dc::lan {

ArrivalClock::ArrivalClock(Config config)
    : cfg_(config), bucketSeconds_(config.windowSeconds / (kBuckets - 1)),
      peakBucketSeconds_(config.peakWindowSeconds / (kBuckets - 1)) {}

void ArrivalClock::reset() {
    min_.fill({});
    peak_.fill({});
    haveBase_ = false;
    base_ = 0;
}

double ArrivalClock::update(std::uint64_t endPos, double t) {
    const double x = t - double(endPos) / cfg_.nominalRate;

    // sliding minimum: one bucket per bucketSeconds of arrival time, the window spans kBuckets - 1
    // full buckets plus the current one
    const auto idx = static_cast<std::int64_t>(std::floor(t / bucketSeconds_));
    auto& b = min_[std::size_t(idx % kBuckets)];
    if (b.index != idx) b = {idx, x};
    else b.value = std::min(b.value, x);
    double m = x;
    for (const auto& e : min_)
        if (e.index > idx - kBuckets && e.index <= idx) m = std::min(m, e.value);
    base_ = m;
    haveBase_ = true;

    const double late = x - base_;
    const auto pidx = static_cast<std::int64_t>(std::floor(t / peakBucketSeconds_));
    auto& p = peak_[std::size_t(pidx % kBuckets)];
    if (p.index != pidx) p = {pidx, late};
    else p.value = std::max(p.value, late);
    return late;
}

double ArrivalClock::latenessPeak(double t) const {
    const auto pidx = static_cast<std::int64_t>(std::floor(t / peakBucketSeconds_));
    double m = 0;
    for (const auto& e : peak_)
        if (e.index > pidx - kBuckets && e.index <= pidx) m = std::max(m, e.value);
    return m;
}

} // namespace dc::lan
