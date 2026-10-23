#include "dc/clocksync/drift_controller.hpp"

#include <algorithm>
#include <cmath>

namespace dc::clocksync {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

DriftController::DriftController(Config config) : cfg_(config) {}

void DriftController::reset(bool keepRatio) {
    filtered_ = 0;
    elapsed_ = 0;
    first_ = true;
    if (!keepRatio) integral_ = 0;
    deviation_ = integral_;
}

double DriftController::update(double error, double dt) {
    if (first_) {
        filtered_ = error;
        first_ = false;
    } else {
        filtered_ += (error - filtered_) * (1 - std::exp(-dt / cfg_.filterSeconds));
    }
    elapsed_ += dt;

    double bw = cfg_.trackBandwidthHz;
    if (elapsed_ < cfg_.lockSeconds) {
        bw = cfg_.lockBandwidthHz;
    } else if (elapsed_ < 2 * cfg_.lockSeconds) {
        const double k = (elapsed_ - cfg_.lockSeconds) / cfg_.lockSeconds;
        bw = cfg_.lockBandwidthHz + (cfg_.trackBandwidthHz - cfg_.lockBandwidthHz) * k;
    }
    const double wn = 2 * kPi * bw;
    const double ki = wn * wn / cfg_.nominalRate;
    const double kp = 2 * cfg_.damping * wn / cfg_.nominalRate;

    integral_ = std::clamp(integral_ + ki * filtered_ * dt, -cfg_.maxDeviation, cfg_.maxDeviation);
    deviation_ = std::clamp(kp * filtered_ + integral_, -cfg_.maxDeviation, cfg_.maxDeviation);
    return 1.0 + deviation_;
}

} // namespace dc::clocksync
