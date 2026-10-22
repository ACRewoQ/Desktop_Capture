#include "common.hpp"

#include <vector>

namespace v6 {

std::string hrText(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    std::string out = buf;
    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    if (n && msg) {
        std::string text = toUtf8(msg);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
        out += " " + text;
        LocalFree(msg);
    }
    return out;
}

std::int64_t nowUs() {
    static const std::int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<std::int64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const std::int64_t t = c.QuadPart;
    return (t / freq) * 1'000'000 + (t % freq) * 1'000'000 / freq;
}

std::string toUtf8(const wchar_t* s) {
    if (!s || !*s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::int64_t processCpuUs() {
    FILETIME create, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) return 0;
    auto toUs = [](const FILETIME& ft) {
        return static_cast<std::int64_t>((static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) /
               10;
    };
    return toUs(kernel) + toUs(user);
}

}  // namespace v6
