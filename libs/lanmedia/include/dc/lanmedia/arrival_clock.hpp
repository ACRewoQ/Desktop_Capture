#pragma once

#include <array>
#include <cstdint>

namespace dc::lan {

// Tracks when a stream's data arrives, on the receiver's clock.
//
// For each packet, x = arrival time - (end position of its data / nominal rate) is the packet's
// one-way delay plus a constant. Queuing in switches, Wi-Fi retries and bursty senders only ever make
// a packet later, never earlier, so the lower envelope of x (its minimum over a sliding window) is
// the delay of the fastest path. Unlike a DLL fed with arrival times, the envelope is not pulled
// around by delay spikes; it follows the slow clock difference between the two PCs within the window
// length.
//
//   availableAt(t) = source frames that a packet taking the fastest path would have delivered by t
//   lateness       = how much later than the envelope a packet arrived
//
// Single-threaded (the receiver's consumer thread). No allocation.
class ArrivalClock {
public:
    struct Config {
        double nominalRate = 48000;
        double windowSeconds = 2;       // envelope window
        double peakWindowSeconds = 20;  // lateness peak window (how long a delay spike is remembered)
    };

    ArrivalClock() : ArrivalClock(Config{}) {}
    explicit ArrivalClock(Config config);

    void reset();

    // A packet whose data ends at source frame endPos arrived at time t (seconds). Returns its
    // lateness in seconds (>= 0).
    double update(std::uint64_t endPos, double t);

    bool ready() const { return haveBase_; }
    double availableAt(double t) const { return (t - base_) * cfg_.nominalRate; }
    // Largest lateness seen within the peak window, seconds.
    double latenessPeak(double t) const;

private:
    static constexpr int kBuckets = 8;

    struct Bucket {
        std::int64_t index = -1;
        double value = 0;
    };

    Config cfg_;
    double bucketSeconds_;
    double peakBucketSeconds_;
    std::array<Bucket, kBuckets> min_{};
    std::array<Bucket, kBuckets> peak_{};
    bool haveBase_ = false;
    double base_ = 0;
};

} // namespace dc::lan
