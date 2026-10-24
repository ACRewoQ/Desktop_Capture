// Tiny file logger usable from the camera DLLs (from the V4 verification).
// The MF source runs in the Frame Server service, where no console or debugger is attached, so
// it writes to %ProgramData%\DesktopCapture\vcam-logs\<name>.log (the register script creates the
// folder writable for services), falling back to %TEMP%.
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace dc::vcam {

inline std::wstring log_path(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring dir = std::wstring(buf) + L"\\DesktopCapture\\vcam-logs";
        if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return dir + L"\\" + name + L".log";
    }
    n = GetTempPathW(MAX_PATH, buf);
    return std::wstring(n ? buf : L".\\") + name + L".log";
}

inline void vlog(const wchar_t* name, const char* fmt, va_list ap) {
    static std::mutex m;
    std::lock_guard lock(m);
    static const std::wstring path = log_path(name);
    FILE* f = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(f, "%02u:%02u:%02u.%03u [pid %lu tid %lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                 GetCurrentProcessId(), GetCurrentThreadId());
    std::vfprintf(f, fmt, ap);
    std::fputc('\n', f);
    std::fclose(f);
}

}  // namespace dc::vcam
