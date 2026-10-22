// Shared helpers for the V6 preview playback verification (Windows only).
#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace v6 {

using Microsoft::WRL::ComPtr;

// Thrown by check() with the failing expression and the HRESULT in hex.
class HrError : public std::runtime_error {
public:
    HrError(const std::string& what, HRESULT hr) : std::runtime_error(what), hr_(hr) {}
    HRESULT hr() const { return hr_; }

private:
    HRESULT hr_;
};

std::string hrText(HRESULT hr);

inline void checkHr(HRESULT hr, const char* expr, const char* file, int line) {
    if (FAILED(hr)) {
        char buf[512];
        std::snprintf(buf, sizeof buf, "%s failed: %s (%s:%d)", expr, hrText(hr).c_str(), file, line);
        throw HrError(buf, hr);
    }
}

#define V6_CHECK(expr) ::v6::checkHr((expr), #expr, __FILE__, __LINE__)

// QueryPerformanceCounter in microseconds. Every stage timestamp uses this clock.
std::int64_t nowUs();

// Narrow conversion for logging names that come back from Media Foundation as UTF-16.
std::string toUtf8(const wchar_t* s);

// Process CPU time (user + kernel) in microseconds, for CPU load figures.
std::int64_t processCpuUs();

}  // namespace v6
