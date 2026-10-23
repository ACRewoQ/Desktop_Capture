#include "dc/clocksync/audio_fifo.hpp"

#include <algorithm>
#include <cstring>

namespace dc::clocksync {

AudioFifo::AudioFifo(int channels, std::size_t capacityFrames) : channels_(channels) {
    std::size_t cap = 1;
    while (cap < capacityFrames) cap <<= 1;
    mask_ = cap - 1;
    data_.assign(cap * std::size_t(channels), 0.0f);
}

std::size_t AudioFifo::available() const {
    return std::size_t(write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire));
}

std::size_t AudioFifo::push(const float* frames, std::size_t count) {
    const auto w = write_.load(std::memory_order_relaxed);
    const auto r = read_.load(std::memory_order_acquire);
    const std::size_t space = capacity() - std::size_t(w - r);
    const std::size_t n = std::min(count, space);
    const std::size_t start = std::size_t(w) & mask_;
    const std::size_t first = std::min(n, capacity() - start);
    std::memcpy(&data_[start * channels_], frames, first * channels_ * sizeof(float));
    std::memcpy(data_.data(), frames + first * channels_, (n - first) * channels_ * sizeof(float));
    write_.store(w + n, std::memory_order_release);
    return n;
}

std::size_t AudioFifo::pop(float* frames, std::size_t count) {
    const auto r = read_.load(std::memory_order_relaxed);
    const auto w = write_.load(std::memory_order_acquire);
    const std::size_t n = std::min(count, std::size_t(w - r));
    const std::size_t start = std::size_t(r) & mask_;
    const std::size_t first = std::min(n, capacity() - start);
    std::memcpy(frames, &data_[start * channels_], first * channels_ * sizeof(float));
    std::memcpy(frames + first * channels_, data_.data(), (n - first) * channels_ * sizeof(float));
    read_.store(r + n, std::memory_order_release);
    return n;
}

std::size_t AudioFifo::skip(std::size_t count) {
    const auto r = read_.load(std::memory_order_relaxed);
    const auto w = write_.load(std::memory_order_acquire);
    const std::size_t n = std::min(count, std::size_t(w - r));
    read_.store(r + n, std::memory_order_release);
    return n;
}

} // namespace dc::clocksync
