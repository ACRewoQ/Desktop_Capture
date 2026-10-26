#pragma once
// dc-tray's decisions that do not need Windows: when to restart a process that ended, how to quote its command
// line, the optional settings file, the autostart state Windows keeps, log rotation and the status file. Tested
// on every platform (tests/test_tray.cpp); the Windows parts (tray icon, processes, registry) use these.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace dc::tray {

// --- Restarting a process that ended on its own -------------------------------------------------------------------

struct RestartPolicy {
    std::int64_t stableMs = 30000;                  // a run at least this long starts the count of failures again
    std::vector<std::int64_t> delaysMs = {1000, 3000, 10000};  // wait before the 1st, 2nd, 3rd restart in a row
    int maxFailures = 3;  // restarts in a row (each run shorter than stableMs); the next failure gives up
};

class RestartTracker {
public:
    explicit RestartTracker(RestartPolicy p = {}) : policy_(std::move(p)) {}
    // The process ended without being asked to after running uptimeMs. Returns the wait before starting it
    // again, or -1 when it keeps failing and should be left stopped.
    std::int64_t onExit(std::int64_t uptimeMs);
    void reset() { failures_ = 0; }
    int failures() const { return failures_; }

private:
    RestartPolicy policy_;
    int failures_ = 0;
};

// --- Command lines -----------------------------------------------------------------------------------------------

// One argument quoted the way CommandLineToArgvW and the MSVC runtime read it back.
std::string quoteArg(const std::string& arg);
// "exe" followed by the arguments; extra (already a command-line fragment, from the settings file) goes last.
std::string buildCommandLine(const std::string& exe, const std::vector<std::string>& args, const std::string& extra = {});

// --- Settings file (%APPDATA%\DesktopCapture\tray.txt, optional) ---------------------------------------------------
//
//   # comment
//   mixd_args = --no-asio          extra arguments for the engine
//   capd_args = --no-video         extra arguments for the capture process
//   ui_args = --page video         extra arguments for the mixer window
//   capd = off                     do not start capd (audio only)

struct TrayConfig {
    std::string mixdArgs;
    std::string capdArgs;
    std::string uiArgs;
    bool capd = true;
};

// Unknown keys and bad values are reported in errors (one line each) and otherwise ignored.
TrayConfig parseConfig(const std::string& text, std::vector<std::string>* errors = nullptr);

// --- Autostart ------------------------------------------------------------------------------------------------------

// The value Windows keeps under Explorer\StartupApproved\Run when the user turns an entry off or on in Settings >
// Apps > Startup (or Task Manager). No value means on; otherwise the lowest bit of the first byte is set when off.
bool startupApproved(const std::vector<std::uint8_t>& value);

// --- Logs -----------------------------------------------------------------------------------------------------------

// Before a process starts again: name.log -> name.1.log -> ... -> name.<keep-1>.log (the oldest is removed).
void rotateLogs(const std::filesystem::path& dir, const std::string& name, int keep);

// --- What the tray shows ----------------------------------------------------------------------------------------------

enum class ChildState { Stopped, Starting, Running, Restarting, Failed, External };

const char* stateKey(ChildState s);       // "running": status file
const char* stateLabel(ChildState s);     // "動作中": menu and tooltip

struct ChildStatus {
    std::string name;  // mixd, capd
    ChildState state = ChildState::Stopped;
    std::uint32_t pid = 0;
    int restarts = 0;  // since the tray started
    bool hasExit = false;
    std::uint32_t lastExit = 0;
};

// Exit code in words for the notification: "終了コード 2", "異常終了（0xC0000005 アクセス違反）".
std::string describeExit(std::uint32_t code);

// %LOCALAPPDATA%\DesktopCapture\tray-status.txt, read by scripts/p5b-tray.ps1:
//   tray_pid=1234
//   mixd.state=running
//   mixd.pid=5678
//   mixd.restarts=0
//   mixd.last_exit=0xC0000005     (only after it ended once)
std::string formatStatus(std::uint32_t trayPid, const std::vector<ChildStatus>& children);

// The colour of the tray icon's fader knobs: every process running, something starting, something failed.
enum class Health { Ok, Busy, Failed, Stopped };
Health overallHealth(const std::vector<ChildStatus>& children);

} // namespace dc::tray
