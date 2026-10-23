// Small helpers shared by the V1 loopback probe: COM pointer, HRESULT text, clocks, UTF-8 output.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace v1 {

// Minimal COM smart pointer (the probe avoids WRL/ATL so it reads the same everywhere).
template <class T>
class Com {
public:
    Com() = default;
    Com(const Com& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Com(Com&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    Com& operator=(Com o) noexcept { std::swap(p_, o.p_); return *this; }
    ~Com() { reset(); }

    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** put() { reset(); return &p_; }
    void** putVoid() { return reinterpret_cast<void**>(put()); }

private:
    T* p_ = nullptr;
};

inline std::string hrText(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    std::string s = buf;
    switch (hr) {
        case E_NOTIMPL: s += " E_NOTIMPL"; break;
        case E_INVALIDARG: s += " E_INVALIDARG"; break;
        case E_ACCESSDENIED: s += " E_ACCESSDENIED"; break;
        case static_cast<HRESULT>(0x88890008L): s += " AUDCLNT_E_UNSUPPORTED_FORMAT"; break;
        case static_cast<HRESULT>(0x88890004L): s += " AUDCLNT_E_DEVICE_INVALIDATED"; break;
        case static_cast<HRESULT>(0x88890026L): s += " AUDCLNT_E_RESOURCES_INVALIDATED"; break;
        case static_cast<HRESULT>(0x80070490L): s += " ERROR_NOT_FOUND"; break;
        default: break;
    }
    return s;
}

inline std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// QueryPerformanceCounter in 100 ns units, the same unit IAudioCaptureClient::GetBuffer reports.
inline std::uint64_t nowHns() {
    static const std::uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return std::uint64_t(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const std::uint64_t t = std::uint64_t(c.QuadPart);
    return (t / freq) * 10'000'000ull + (t % freq) * 10'000'000ull / freq;
}

// Windows version straight from ntdll (GetVersionEx lies without a manifest).
struct OsVersion {
    unsigned long major = 0, minor = 0, build = 0, ubr = 0;
    std::string displayVersion;  // e.g. "23H2"
};

inline OsVersion osVersion() {
    OsVersion v;
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof info;
        if (fn && fn(&info) == 0) {
            v.major = info.dwMajorVersion;
            v.minor = info.dwMinorVersion;
            v.build = info.dwBuildNumber;
        }
    }
    DWORD ubr = 0, size = sizeof ubr;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
                     nullptr, &ubr, &size) == ERROR_SUCCESS)
        v.ubr = ubr;
    wchar_t disp[64] = {};
    DWORD dsize = sizeof disp;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion",
                     RRF_RT_REG_SZ, nullptr, disp, &dsize) == ERROR_SUCCESS)
        v.displayVersion = utf8(disp);
    return v;
}

inline std::string processName(DWORD pid) {
    std::wstring name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
            name.assign(path, len);
            const auto slash = name.find_last_of(L'\\');
            if (slash != std::wstring::npos) name = name.substr(slash + 1);
        }
        CloseHandle(h);
    }
    return name.empty() ? std::string("?") : utf8(name);
}

}  // namespace v1
