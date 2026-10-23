#pragma once

#include <cstdint>

namespace dc::clocksync {

// Second-order delay-locked loop (F. Adriaensen, "Using a DLL to filter time") that maps a device's
// frame position to a smoothed time. Fed with (frame position, timestamp) pairs from the device's
// packets, it removes timestamp jitter and the sawtooth of bursty delivery, so positionAt() gives the
// device's frame position at any instant.
class TimeDll {
public:
    // bandwidthHz: lower = smoother, slower to follow a rate change. 0.5 Hz suits 10 ms packets.
    explicit TimeDll(double nominalRate = 48000, double bandwidthHz = 0.5);

    void reset();

    // framePos: device frame index of the first frame of the packet; seconds: when that frame was
    // sampled (or will be played). Packets must arrive in order. A jump of more than maxErrorSeconds
    // restarts the loop (device glitch, discontinuity).
    void update(std::uint64_t framePos, double seconds);

    bool ready() const { return updates_ >= 2; }
    std::uint64_t updates() const { return updates_; }

    // Smoothed frame position at the given time (extrapolated from the last packet).
    double positionAt(double seconds) const;

    // Smoothed time of the given frame position (inverse of positionAt).
    double timeAt(double framePos) const;

    // Estimated frames per second of the device clock.
    double rate() const { return 1.0 / secondsPerFrame_; }

    double maxErrorSeconds = 0.05;

private:
    double nominalRate_;
    double bandwidth_;
    std::uint64_t updates_ = 0;
    std::uint64_t refPos_ = 0;
    double refTime_ = 0;
    double secondsPerFrame_;
};

} // namespace dc::clocksync
