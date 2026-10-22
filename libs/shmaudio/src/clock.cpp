#include "dc/shmaudio/clock.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace dc::shm {

#ifdef _WIN32
std::int64_t nowQpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

std::int64_t qpcFrequency() {
    static const std::int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}
#else
std::int64_t nowQpc() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

std::int64_t qpcFrequency() { return 1'000'000'000; }
#endif

} // namespace dc::shm
