#pragma once
// Clean stop for a process started without a console. dc-tray starts mixd and capd hidden, so it cannot send
// them Ctrl+C; it sets a named event instead ("--stop-event NAME") and the process stops as it does on Ctrl+C.
// Outside Windows there are no named events and the watcher does nothing (Ctrl+C / SIGTERM still work).

#include <functional>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dc::control {

class StopEventWatcher {
public:
    StopEventWatcher() = default;
    // Calls onStop once, on the watcher's own thread, when the event NAME is set. An empty name watches nothing.
    StopEventWatcher(const std::string& name, std::function<void()> onStop) {
#ifdef _WIN32
        if (name.empty()) return;
        const int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide.data(), n);
        // Manual reset, not signalled: opens the starter's event when it exists, creates it otherwise.
        stop_ = CreateEventW(nullptr, TRUE, FALSE, wide.c_str());
        quit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_ || !quit_) return;
        thread_ = std::thread([this, cb = std::move(onStop)] {
            HANDLE handles[2] = {quit_, stop_};
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1 && cb) cb();
        });
#else
        (void)name;
        (void)onStop;
#endif
    }
    ~StopEventWatcher() {
#ifdef _WIN32
        if (quit_) SetEvent(quit_);
        if (thread_.joinable()) thread_.join();
        if (stop_) CloseHandle(stop_);
        if (quit_) CloseHandle(quit_);
#endif
    }
    StopEventWatcher(const StopEventWatcher&) = delete;
    StopEventWatcher& operator=(const StopEventWatcher&) = delete;

private:
#ifdef _WIN32
    HANDLE stop_ = nullptr;
    HANDLE quit_ = nullptr;
#endif
    std::thread thread_;
};

} // namespace dc::control
