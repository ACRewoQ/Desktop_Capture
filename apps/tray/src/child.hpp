#pragma once
// One supervised process (mixd or capd): started hidden with its output in a log file, watched from the thread
// pool (its end posts a message to the tray window), stopped cleanly through a named event.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dc::tray {

class Child {
public:
    struct Options {
        std::string name;                   // mixd, capd: log file and stop event name
        std::filesystem::path exe;
        std::vector<std::string> args;      // after the --stop-event argument
        std::string extraArgs;              // from tray.txt, already a command-line fragment
        std::filesystem::path logDir;
        HWND notify = nullptr;              // gets exitMessage(wParam = id, lParam = generation) when it ends
        UINT exitMessage = 0;
        WPARAM id = 0;
    };

    explicit Child(Options o);
    ~Child();
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    const Options& options() const { return o_; }
    std::wstring stopEventName() const;

    // Starts it (the previous logs move one place down). False with err when CreateProcess fails.
    bool start(std::string* err);
    // Takes over a process of the same exe that is already running (left by an earlier tray that ended without
    // stopping it, or started by hand from the same folder). False when there is none.
    bool adopt();

    bool running() const { return process_ != nullptr; }
    DWORD pid() const { return pid_; }
    std::uint64_t generation() const { return generation_; }
    bool adopted() const { return adopted_; }

    // Asks it to stop (the event), waits up to timeoutMs, then terminates it. Returns its exit code.
    DWORD stop(DWORD timeoutMs);
    // Starts a clean stop without waiting (several children stop in parallel); finish with stop().
    void requestStop();

    // After the exit message of the current generation: closes the handles and gives the exit code and how
    // long it ran. False when the message is stale (already collected or a newer run).
    bool collectExit(std::uint64_t generation, DWORD* exitCode, std::int64_t* uptimeMs);

private:
    void watch();
    void release();
    static void CALLBACK onExit(void* context, BOOLEAN timedOut);

    Options o_;
    HANDLE stopEvent_ = nullptr;
    HANDLE process_ = nullptr;
    HANDLE wait_ = nullptr;
    DWORD pid_ = 0;
    std::atomic<std::uint64_t> generation_{0};  // read by the pool thread's callback
    ULONGLONG startedMs_ = 0;
    bool adopted_ = false;
};

// The window of a process (top level, visible), or nullptr.
HWND findProcessWindow(DWORD pid);

} // namespace dc::tray
