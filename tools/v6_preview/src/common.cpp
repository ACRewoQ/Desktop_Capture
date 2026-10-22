#include "common.hpp"

#include <dbghelp.h>

#include <cstring>
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

namespace {

LONG WINAPI reportCrash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    HANDLE proc = GetCurrentProcess(), thread = GetCurrentThread();
    std::fprintf(stderr, "CRASH: exception 0x%08lX at %p (thread %lu)\n",
                 static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode), ep->ExceptionRecord->ExceptionAddress,
                 static_cast<unsigned long>(GetCurrentThreadId()));
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 f{};
    f.AddrPC.Offset = ctx.Rip;
    f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp;
    f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp;
    f.AddrStack.Mode = AddrModeFlat;
    // A call through a bad pointer leaves Rip unusable; the caller's return address is at [Rsp].
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ctx.Rip &&
        ctx.Rip == reinterpret_cast<DWORD64>(ep->ExceptionRecord->ExceptionAddress)) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(ctx.Rip), &mbi, sizeof mbi) || !(mbi.Protect & 0xF0)) {
            const DWORD64 ret = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            std::fprintf(stderr, "  (pc not executable; return address %p)\n", reinterpret_cast<void*>(ret));
            ctx.Rip = ret;
            ctx.Rsp += 8;
            f.AddrPC.Offset = ctx.Rip;
            f.AddrStack.Offset = ctx.Rsp;
        }
    }
    for (int i = 0; i < 40; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &f, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) ||
            !f.AddrPC.Offset)
            break;
        const DWORD64 pc = f.AddrPC.Offset;
        char modName[MAX_PATH] = "?";
        const DWORD64 modBase = SymGetModuleBase64(proc, pc);
        if (modBase) GetModuleFileNameA(reinterpret_cast<HMODULE>(modBase), modName, MAX_PATH);
        const char* shortMod = std::strrchr(modName, '\\') ? std::strrchr(modName, '\\') + 1 : modName;
        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof line;
        DWORD lineDisp = 0;
        const bool haveSym = SymFromAddr(proc, pc, &disp, sym);
        const bool haveLine = SymGetLineFromAddr64(proc, pc, &lineDisp, &line);
        std::fprintf(stderr, "  #%02d %s+0x%llx %s%s%s:%lu\n", i, shortMod,
                     static_cast<unsigned long long>(modBase ? pc - modBase : pc), haveSym ? sym->Name : "",
                     haveLine ? " " : "", haveLine ? line.FileName : "", haveLine ? line.LineNumber : 0ul);
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void installCrashReporter() { SetUnhandledExceptionFilter(reportCrash); }

}  // namespace v6
