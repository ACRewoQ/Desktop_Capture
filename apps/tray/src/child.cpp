#include "child.hpp"

#include "tray_core.hpp"
#include "win_util.hpp"

#include <tlhelp32.h>

#include <algorithm>
#include <iterator>
#include <memory>

namespace dc::tray {

Child::Child(Options o) : o_(std::move(o)) {
    // Manual reset; reset before every start, because a set event outlives the process that read it.
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, stopEventName().c_str());
}

Child::~Child() {
    release();
    if (stopEvent_) CloseHandle(stopEvent_);
}

std::wstring Child::stopEventName() const { return L"Local\\DesktopCapture.stop." + widen(o_.name); }

bool Child::start(std::string* err) {
    release();
    if (stopEvent_) ResetEvent(stopEvent_);
    rotateLogs(o_.logDir, o_.name, 5);

    std::vector<std::string> args = {"--stop-event", narrow(stopEventName())};
    args.insert(args.end(), o_.args.begin(), o_.args.end());
    const std::wstring cmd = widen(buildCommandLine(narrow(o_.exe.wstring()), args, o_.extraArgs));

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    const std::filesystem::path logPath = o_.logDir / widen(o_.name + ".log");
    HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (log == INVALID_HANDLE_VALUE || nul == INVALID_HANDLE_VALUE) {
        if (err) *err = "cannot open " + narrow(logPath.wstring()) + ": " + errorText(GetLastError());
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        return false;
    }
    const std::string header = "--- started by dc-tray: " + narrow(cmd) + "\r\n";
    DWORD written = 0;
    WriteFile(log, header.data(), static_cast<DWORD>(header.size()), &written, nullptr);

    // Only these two handles reach the child (not the tray's other inheritable handles, if any).
    HANDLE inherit[2] = {log, nul};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::unique_ptr<std::byte[]> buf(new std::byte[size]);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf.get());
    InitializeProcThreadAttributeList(attrs, 1, 0, &size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd;
    const std::wstring dir = o_.exe.parent_path().wstring();
    const BOOL ok = CreateProcessW(o_.exe.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, dir.c_str(),
                                   &si.StartupInfo, &pi);
    const DWORD error = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(log);
    CloseHandle(nul);
    if (!ok) {
        if (err) *err = narrow(o_.exe.wstring()) + ": " + errorText(error);
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    startedMs_ = GetTickCount64();
    adopted_ = false;
    watch();
    return true;
}

bool Child::adopt() {
    release();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    const std::wstring wanted = o_.exe.filename().wstring();
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL more = Process32FirstW(snap, &pe); more && !process_; more = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, wanted.c_str()) != 0) continue;
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t image[MAX_PATH * 2];
        DWORD n = static_cast<DWORD>(std::size(image));
        // Only the same file: a test build of mixd running from the build folder is left alone.
        if (QueryFullProcessImageNameW(h, 0, image, &n) && _wcsicmp(image, o_.exe.c_str()) == 0) {
            process_ = h;
            pid_ = pe.th32ProcessID;
            FILETIME created, exited, kernel, user;
            startedMs_ = GetTickCount64();
            if (GetProcessTimes(h, &created, &exited, &kernel, &user)) {
                FILETIME now;
                GetSystemTimeAsFileTime(&now);
                const auto ticks = [](FILETIME f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
                const ULONGLONG ageMs = (ticks(now) - ticks(created)) / 10000;
                startedMs_ -= std::min(startedMs_, ageMs);
            }
        } else {
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    if (!process_) return false;
    if (stopEvent_) ResetEvent(stopEvent_);
    adopted_ = true;
    watch();
    return true;
}

void Child::watch() {
    ++generation_;
    // The callback runs on a pool thread; it only posts, the tray window does the rest.
    RegisterWaitForSingleObject(&wait_, process_, &Child::onExit, this, INFINITE, WT_EXECUTEONLYONCE);
}

void CALLBACK Child::onExit(void* context, BOOLEAN) {
    auto* self = static_cast<Child*>(context);
    PostMessageW(self->o_.notify, self->o_.exitMessage, self->o_.id, static_cast<LPARAM>(self->generation_.load()));
}

void Child::requestStop() {
    if (process_ && stopEvent_) SetEvent(stopEvent_);
}

DWORD Child::stop(DWORD timeoutMs) {
    if (!process_) return 0;
    requestStop();
    if (WaitForSingleObject(process_, timeoutMs) != WAIT_OBJECT_0) {
        logLine(o_.name + ": no clean stop within " + std::to_string(timeoutMs) + " ms, terminating");
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, 5000);
    }
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    release();
    return code;
}

bool Child::collectExit(std::uint64_t generation, DWORD* exitCode, std::int64_t* uptimeMs) {
    if (!process_ || generation != generation_) return false;
    if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) return false;
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    if (exitCode) *exitCode = code;
    if (uptimeMs) *uptimeMs = static_cast<std::int64_t>(GetTickCount64() - startedMs_);
    release();
    return true;
}

void Child::release() {
    if (wait_) {
        UnregisterWaitEx(wait_, INVALID_HANDLE_VALUE);  // waits for a running callback
        wait_ = nullptr;
    }
    if (process_) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    pid_ = 0;
    ++generation_;  // an exit message already posted for this run is now stale
    adopted_ = false;
}

namespace {

struct FindWindowData {
    DWORD pid;
    HWND found;
};

BOOL CALLBACK findWindowProc(HWND hwnd, LPARAM lp) {
    auto* d = reinterpret_cast<FindWindowData*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == d->pid && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
        d->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

} // namespace

HWND findProcessWindow(DWORD pid) {
    FindWindowData d{pid, nullptr};
    EnumWindows(findWindowProc, reinterpret_cast<LPARAM>(&d));
    return d.found;
}

} // namespace dc::tray
