#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dc::clocksync {

// Single-producer / single-consumer FIFO of interleaved float frames. Lock-free, no allocation after
// construction. Counters are absolute frame counts since construction.
class AudioFifo {
public:
    AudioFifo(int channels, std::size_t capacityFrames); // capacity is rounded up to a power of two

    // Producer. Returns frames stored (fewer than requested when full).
    std::size_t push(const float* frames, std::size_t count);
    // Consumer.
    std::size_t pop(float* frames, std::size_t count);
    // Consumer: drops up to count frames.
    std::size_t skip(std::size_t count);

    std::size_t available() const; // frames readable now
    std::size_t capacity() const { return mask_ + 1; }
    std::uint64_t written() const { return write_.load(std::memory_order_acquire); }
    std::uint64_t read() const { return read_.load(std::memory_order_acquire); }
    int channels() const { return channels_; }

private:
    int channels_;
    std::size_t mask_;
    std::vector<float> data_;
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
};

// SPSC queue of small trivially copyable records (timestamps from the producer to the consumer).
template <typename T, std::size_t N> class SpscQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    bool push(const T& v) {
        const auto w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= N) return false;
        items_[w & (N - 1)] = v;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& v) {
        const auto r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        v = items_[r & (N - 1)];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

private:
    T items_[N]{};
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
};

} // namespace dc::clocksync
