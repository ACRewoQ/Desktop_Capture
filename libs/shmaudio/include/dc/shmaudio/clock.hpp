#pragma once

#include <cstdint>

namespace dc::shm {

// QueryPerformanceCounter on Windows; CLOCK_MONOTONIC in nanoseconds elsewhere (tests only).
std::int64_t nowQpc();
std::int64_t qpcFrequency();

} // namespace dc::shm
