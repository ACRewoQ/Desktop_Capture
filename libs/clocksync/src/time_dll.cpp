#include "dc/clocksync/time_dll.hpp"

#include <cmath>

namespace dc::clocksync {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

TimeDll::TimeDll(double nominalRate, double bandwidthHz)
    : nominalRate_(nominalRate), bandwidth_(bandwidthHz), secondsPerFrame_(1.0 / nominalRate) {}

void TimeDll::reset() {
    updates_ = 0;
    secondsPerFrame_ = 1.0 / nominalRate_;
}

void TimeDll::update(std::uint64_t framePos, double seconds) {
    if (updates_ > 0 && framePos > refPos_) {
        const double frames = double(framePos - refPos_);
        const double predicted = refTime_ + frames * secondsPerFrame_;
        const double e = seconds - predicted;
        if (std::abs(e) <= maxErrorSeconds) {
            // loop coefficients for this update interval (packets need not be equal in size)
            const double omega = 2 * kPi * bandwidth_ * frames * secondsPerFrame_;
            refTime_ = predicted + std::sqrt(2.0) * omega * e;
            refPos_ = framePos;
            secondsPerFrame_ += omega * omega * e / frames;
            ++updates_;
            return;
        }
    } else if (updates_ > 0 && framePos == refPos_) {
        return; // duplicate packet position carries no information
    }
    // first packet, or a jump the loop must not follow: restart from this packet
    const bool keepRate = updates_ >= 2;
    updates_ = 1;
    refPos_ = framePos;
    refTime_ = seconds;
    if (!keepRate) secondsPerFrame_ = 1.0 / nominalRate_;
}

double TimeDll::positionAt(double seconds) const {
    return double(refPos_) + (seconds - refTime_) / secondsPerFrame_;
}

} // namespace dc::clocksync
