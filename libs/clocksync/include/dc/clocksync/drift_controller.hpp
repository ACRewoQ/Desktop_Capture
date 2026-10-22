#pragma once

namespace dc::clocksync {

// PI controller that turns the error of a FIFO fill level (in frames, positive = too full) into a
// resampling ratio (input frames per output frame). The fill is first low-pass filtered.
//
// Plant: fill' = rate * (drift - (ratio - 1)), so with u = ratio - 1 = Kp*e + Ki*integral(e) the loop is
// a second-order system with natural frequency wn = sqrt(rate*Ki) and damping zeta = rate*Kp / (2*wn).
// The loop starts wide (fast lock) and narrows to its tracking bandwidth after lockSeconds.
class DriftController {
public:
    struct Config {
        double nominalRate = 48000;
        double lockBandwidthHz = 0.05;  // bandwidth while locking
        double trackBandwidthHz = 0.01; // bandwidth once locked (slower = smoother pitch)
        double lockSeconds = 15;        // time at lock bandwidth, then a linear move of the same length
        double damping = 1.0;
        double filterSeconds = 0.5; // time constant of the fill low-pass
        double maxDeviation = 2e-3; // ratio clamp: 1 +- 2000 ppm
    };

    DriftController() : DriftController(Config{}) {}
    explicit DriftController(Config config);

    // Starts a new lock. With keepRatio the learned clock difference is kept (resync after a glitch).
    void reset(bool keepRatio = false);

    // error: fill - setpoint in frames; dt: seconds since the previous update. Returns the ratio.
    double update(double error, double dt);

    double ratio() const { return 1.0 + deviation_; }
    double filteredError() const { return filtered_; }
    double elapsed() const { return elapsed_; }
    bool locked() const { return elapsed_ >= 2 * cfg_.lockSeconds; }

private:
    Config cfg_;
    double filtered_ = 0;
    double integral_ = 0; // integral term contribution to the deviation
    double deviation_ = 0;
    double elapsed_ = 0;
    bool first_ = true;
};

} // namespace dc::clocksync
