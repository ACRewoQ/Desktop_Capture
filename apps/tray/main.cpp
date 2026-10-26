// dc-tray: the small resident program in the notification area (P5-B). Starts the engine (mixd) and the capture
// process (capd) hidden, watches them (a process that ends on its own is reported and started again), opens the
// mixer window (dc-ui), and turns starting at logon on and off. Plain Win32: no Qt, no WebView.
//
//   dc-tray [--background] [--bin-dir DIR] [--mixd-args "..."] [--capd-args "..."] [--no-capd]
//   dc-tray --autostart on|off|status
//   dc-tray --open | --quit
//   dc-tray --version | --help
//
// Only one runs per user session; starting it again opens the mixer window (or with --quit stops the running one).

#include "src/autostart.hpp"
#include "src/child.hpp"
#include "src/icon.hpp"
#include "src/tray_core.hpp"
#include "src/win_util.hpp"

#include "dc/version.hpp"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <windowsx.h>

#include <memory>
#include <string>
#include <vector>

using namespace dc::tray;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t kWindowClass[] = L"DesktopCapture.Tray";
constexpr wchar_t kMutexName[] = L"Local\\DesktopCapture.Tray";

constexpr UINT WM_TRAY_ICON = WM_APP + 1;
constexpr UINT WM_CHILD_EXIT = WM_APP + 2;
constexpr UINT WM_OPEN_UI = WM_APP + 3;
constexpr UINT WM_QUIT_TRAY = WM_APP + 4;

constexpr UINT kIconId = 1;
constexpr UINT_PTR kRestartTimerBase = 100;  // + child index
constexpr DWORD kStopTimeoutMs = 15000;     // capd closes the recording, mixd unloads plug-ins

enum MenuId : UINT {
    IdOpen = 1,
    IdRestart,
    IdLogs,
    IdSettings,
    IdAutostart,
    IdQuit,
    IdStatusBase = 50,
};

struct Args {
    bool background = false;
    bool noCapd = false;
    bool haveMixdArgs = false, haveCapdArgs = false;
    std::string mixdArgs, capdArgs;
    fs::path binDir;
    std::string autostart;  // on, off, status
    bool open = false, quit = false, version = false, help = false;
    std::string error;
};

Args parseArgs() {
    Args a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        const std::wstring s = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                a.error = narrow(s) + " needs a value";
                return {};
            }
            return narrow(argv[++i]);
        };
        if (s == L"--background") a.background = true;
        else if (s == L"--no-capd") a.noCapd = true;
        else if (s == L"--mixd-args") a.mixdArgs = next(), a.haveMixdArgs = true;
        else if (s == L"--capd-args") a.capdArgs = next(), a.haveCapdArgs = true;
        else if (s == L"--bin-dir") a.binDir = fs::path(widen(next()));
        else if (s == L"--autostart") a.autostart = next();
        else if (s == L"--open") a.open = true;
        else if (s == L"--quit") a.quit = true;
        else if (s == L"--version") a.version = true;
        else if (s == L"--help" || s == L"-h") a.help = true;
        else a.error = "unknown option " + narrow(s);
    }
    LocalFree(argv);
    return a;
}

const char* kUsage =
    "usage: dc-tray [--background] [--bin-dir DIR] [--mixd-args \"...\"] [--capd-args \"...\"] [--no-capd]\n"
    "       dc-tray --autostart on|off|status\n"
    "       dc-tray --open | --quit\n"
    "       dc-tray --version\n"
    "  --background  do not open the mixer window at start (the logon entry uses it)\n"
    "  --bin-dir     folder with mixd.exe, capd.exe and dc-ui.exe (default: this program's folder, or the build tree)\n"
    "  --mixd-args   extra arguments for mixd (instead of mixd_args in tray.txt); --capd-args likewise\n"
    "  --no-capd     audio only: do not start capd\n"
    "  --autostart   start at logon (the per-user Run key) on or off, or print its state\n"
    "  --open        open the mixer window of the running tray (starts the tray when none runs)\n"
    "  --quit        stop the running tray, capd and mixd (waits until they are gone)\n"
    "settings file: %APPDATA%\\DesktopCapture\\tray.txt (mixd_args, capd_args, ui_args, capd = on|off)\n"
    "logs: %LOCALAPPDATA%\\DesktopCapture\\logs, state: %LOCALAPPDATA%\\DesktopCapture\\tray-status.txt\n";

// mixd.exe next to this program (the package), or in the build tree: build\windows\apps\<dir>\<Config>\<exe>.
fs::path findExe(const fs::path& binDir, const wchar_t* exe, const wchar_t* buildDir) {
    std::error_code ec;
    if (!binDir.empty()) return binDir / exe;
    const fs::path here = exePath().parent_path();
    if (fs::exists(here / exe, ec)) return here / exe;
    const fs::path built = here.parent_path().parent_path() / buildDir / here.filename() / exe;
    if (fs::exists(built, ec)) return built;
    return here / exe;
}

constexpr char kSettingsTemplate[] =
    "# dc-tray settings. Lines are key = value; # starts a comment. Takes effect with \"Restart\" in the tray menu.\r\n"
    "#\r\n"
    "# mixd_args = --no-asio          extra arguments for the engine (mixd --help)\r\n"
    "# capd_args = --no-video         extra arguments for the capture process (capd --help)\r\n"
    "# ui_args = --page video         extra arguments for the mixer window (dc-ui --help)\r\n"
    "# capd = off                     audio only: do not start capd\r\n";

class TrayApp {
public:
    explicit TrayApp(Args args) : args_(std::move(args)) {}

    int run(HINSTANCE inst) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = &TrayApp::wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc);
        // A hidden top-level window (not message-only: those miss the TaskbarCreated broadcast).
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"Desktop Capture", WS_POPUP, 0, 0, 0, 0, nullptr,
                                nullptr, inst, this);
        if (!hwnd_) return 1;
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        logLine("dc-tray " + std::string(dc::kVersionString) + " started (pid " + std::to_string(GetCurrentProcessId()) + ")");

        loadSettings();
        createChildren();
        addIcon();
        startAll();
        if (!args_.background) openUi();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        logLine("dc-tray stopped");
        return 0;
    }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* self = static_cast<TrayApp*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) return self->handle(hwnd, msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == taskbarCreated_ && taskbarCreated_) {  // Explorer restarted: the icon is gone
            iconAdded_ = false;
            addIcon();
            return 0;
        }
        switch (msg) {
        case WM_TRAY_ICON:
            switch (LOWORD(lp)) {
            case NIN_SELECT:
            case NIN_KEYSELECT: openUi(); break;
            case WM_CONTEXTMENU: showMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp)); break;
            case NIN_BALLOONUSERCLICK: openFolder(logDir()); break;
            default: break;
            }
            return 0;
        case WM_CHILD_EXIT: onChildExit(static_cast<size_t>(wp), static_cast<std::uint64_t>(lp)); return 0;
        case WM_TIMER:
            if (wp >= kRestartTimerBase && wp < kRestartTimerBase + children_.size()) {
                KillTimer(hwnd, wp);
                startChild(wp - kRestartTimerBase);
            }
            return 0;
        case WM_OPEN_UI: openUi(); return 0;
        case WM_QUIT_TRAY: quit(); return 0;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION:
            // Windows is shutting down or the user logs off: close the recording and stop the engine before
            // Windows ends them (about 5 s before it asks the user).
            if (wp) {
                logLine("session ends");
                stopAll(4000);
                removeIcon();
            }
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    // --- settings and children -----------------------------------------------------------------------------------

    void loadSettings() {
        const fs::path file = appDataDir() / L"tray.txt";
        std::vector<std::string> errors;
        config_ = parseConfig(readFile(file), &errors);
        for (const auto& e : errors) logLine("tray.txt " + e);
        if (args_.haveMixdArgs) config_.mixdArgs = args_.mixdArgs;
        if (args_.haveCapdArgs) config_.capdArgs = args_.capdArgs;
        if (args_.noCapd) config_.capd = false;
    }

    void createChildren() {
        children_.clear();
        status_.clear();
        trackers_.clear();
        labels_.clear();
        auto add = [&](const char* name, const char* label, const wchar_t* exe, const wchar_t* dir, std::string extra) {
            Child::Options o;
            o.name = name;
            o.exe = findExe(args_.binDir, exe, dir);
            o.extraArgs = std::move(extra);
            o.logDir = logDir();
            o.notify = hwnd_;
            o.exitMessage = WM_CHILD_EXIT;
            o.id = children_.size();
            children_.push_back(std::make_unique<Child>(std::move(o)));
            ChildStatus st;
            st.name = name;
            status_.push_back(st);
            trackers_.emplace_back();
            labels_.push_back(label);
        };
        add("mixd", "エンジン（mixd）", L"mixd.exe", L"mixd", config_.mixdArgs);
        if (config_.capd) add("capd", "キャプチャ（capd）", L"capd.exe", L"capd", config_.capdArgs);
        uiExe_ = findExe(args_.binDir, L"dc-ui.exe", L"ui");
    }

    void startAll() {
        for (size_t i = 0; i < children_.size(); ++i) {
            trackers_[i].reset();
            startChild(i);
        }
    }

    void startChild(size_t i) {
        Child& c = *children_[i];
        ChildStatus& st = status_[i];
        if (quitting_) return;
        if (c.adopt()) {
            st.state = ChildState::Running;
            st.pid = c.pid();
            logLine(st.name + ": already running (pid " + std::to_string(st.pid) + "), watching it");
            update();
            return;
        }
        std::error_code ec;
        if (!fs::exists(c.options().exe, ec)) {
            st.state = ChildState::Failed;
            st.pid = 0;
            logLine(st.name + ": not found: " + narrow(c.options().exe.wstring()));
            notify(labels_[i] + "が見つかりません", narrow(c.options().exe.wstring()), NIIF_ERROR);
            update();
            return;
        }
        if (otherInstanceRunning(c.options().exe)) {
            // A test build (or one from another folder) holds the ports; a second engine would only fail.
            st.state = ChildState::External;
            st.pid = 0;
            logLine(st.name + ": another " + narrow(c.options().exe.filename().wstring()) + " is running, not starting");
            notify(labels_[i] + "は別の場所から起動されています",
                   "トレイからは起動しません。それを止めてから、メニューの「再起動」を押してください。", NIIF_WARNING);
            update();
            return;
        }
        std::string err;
        st.state = ChildState::Starting;
        if (!c.start(&err)) {
            logLine(st.name + ": start failed: " + err);
            onEnded(i, 0, 0, false, err);
            return;
        }
        st.pid = c.pid();
        st.state = ChildState::Running;
        logLine(st.name + ": started (pid " + std::to_string(st.pid) + ")");
        update();
    }

    static bool otherInstanceRunning(const fs::path& exe) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;
        const std::wstring name = exe.filename().wstring();
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof pe;
        bool found = false;
        for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe))
            found = _wcsicmp(pe.szExeFile, name.c_str()) == 0;
        CloseHandle(snap);
        return found;
    }

    void onChildExit(size_t i, std::uint64_t generation) {
        if (i >= children_.size()) return;
        DWORD code = 0;
        std::int64_t uptime = 0;
        if (!children_[i]->collectExit(generation, &code, &uptime)) return;  // stale, or we stopped it
        logLine(status_[i].name + ": ended after " + std::to_string(uptime / 1000) + " s, " + describeExit(code));
        onEnded(i, code, uptime, true, {});
    }

    // The process ended without being asked to (or did not start): restart it after a while, or give up.
    void onEnded(size_t i, DWORD code, std::int64_t uptimeMs, bool ran, const std::string& startError) {
        ChildStatus& st = status_[i];
        st.pid = 0;
        if (ran) {
            st.hasExit = true;
            st.lastExit = code;
        }
        if (quitting_) return;
        const std::string what = ran ? describeExit(code) : "起動できません: " + startError;
        const std::int64_t delay = trackers_[i].onExit(uptimeMs);
        if (delay < 0) {
            st.state = ChildState::Failed;
            logLine(st.name + ": keeps ending, not restarting");
            notify(labels_[i] + "を立ち上げ直せません",
                   what + "。何度も終了するので止めました。ログを見て、メニューの「再起動」で戻せます。", NIIF_ERROR);
        } else {
            st.state = ChildState::Restarting;
            ++st.restarts;
            logLine(st.name + ": restarting in " + std::to_string(delay) + " ms");
            notify(labels_[i] + "が終了しました", what + "。立ち上げ直します。", NIIF_WARNING);
            SetTimer(hwnd_, kRestartTimerBase + i, static_cast<UINT>(delay), nullptr);
        }
        update();
    }

    // capd first: it closes the recording while the engine is still there (without the engine it would mark the
    // recording as failed).
    void stopAll(DWORD timeoutMs) {
        for (size_t k = children_.size(); k-- > 0;) {
            KillTimer(hwnd_, kRestartTimerBase + k);
            if (!children_[k]->running()) {
                if (status_[k].state != ChildState::External) status_[k].state = ChildState::Stopped;
                continue;
            }
            const DWORD code = children_[k]->stop(timeoutMs);
            logLine(status_[k].name + ": stopped, " + describeExit(code));
            status_[k].state = ChildState::Stopped;
            status_[k].pid = 0;
        }
        update();
    }

    void restartAll() {
        logLine("restart requested");
        stopAll(kStopTimeoutMs);
        loadSettings();
        createChildren();  // tray.txt may have changed
        startAll();
    }

    void quit() {
        if (quitting_) return;
        logLine("quit requested");
        closeUi();
        stopAll(kStopTimeoutMs);
        quitting_ = true;
        update();
        removeIcon();
        DestroyWindow(hwnd_);
    }

    // --- mixer window --------------------------------------------------------------------------------------------

    void openUi() {
        if (ui_ && WaitForSingleObject(ui_, 0) == WAIT_TIMEOUT) {
            if (HWND w = findProcessWindow(uiPid_)) {
                if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
                SetForegroundWindow(w);
            }
            return;  // still starting when it has no window yet
        }
        if (ui_) CloseHandle(ui_), ui_ = nullptr;
        std::error_code ec;
        if (!fs::exists(uiExe_, ec)) {
            notify("ミキサーの画面（dc-ui）が見つかりません", narrow(uiExe_.wstring()), NIIF_ERROR);
            return;
        }
        std::wstring cmd = widen(buildCommandLine(narrow(uiExe_.wstring()), {}, config_.uiArgs));
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        const std::wstring dir = uiExe_.parent_path().wstring();
        if (!CreateProcessW(uiExe_.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
            const std::string err = errorText(GetLastError());
            logLine("dc-ui: start failed: " + err);
            notify("ミキサーの画面を開けません", err, NIIF_ERROR);
            return;
        }
        CloseHandle(pi.hThread);
        ui_ = pi.hProcess;
        uiPid_ = pi.dwProcessId;
        logLine("dc-ui: started (pid " + std::to_string(uiPid_) + ")");
    }

    void closeUi() {
        if (!ui_) return;
        if (WaitForSingleObject(ui_, 0) == WAIT_TIMEOUT) {
            if (HWND w = findProcessWindow(uiPid_)) PostMessageW(w, WM_CLOSE, 0, 0);
            WaitForSingleObject(ui_, 3000);
        }
        CloseHandle(ui_);
        ui_ = nullptr;
    }

    // --- tray icon and menu --------------------------------------------------------------------------------------

    NOTIFYICONDATAW iconData() const {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof nid;
        nid.hWnd = hwnd_;
        nid.uID = kIconId;
        return nid;
    }

    void addIcon() {
        NOTIFYICONDATAW nid = iconData();
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = WM_TRAY_ICON;
        nid.hIcon = currentIcon();
        setTip(nid);
        if (!Shell_NotifyIconW(NIM_ADD, &nid)) return;  // no taskbar yet (early logon): TaskbarCreated comes later
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        iconAdded_ = true;
    }

    void removeIcon() {
        if (!iconAdded_) return;
        NOTIFYICONDATAW nid = iconData();
        Shell_NotifyIconW(NIM_DELETE, &nid);
        iconAdded_ = false;
    }

    HICON currentIcon() {
        const Health h = overallHealth(status_);
        auto& icon = icons_[static_cast<int>(h)];
        if (!icon) icon = makeTrayIcon(h, GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()));
        return icon;
    }

    std::string statusLine(size_t i) const { return labels_[i] + ": " + stateLabel(status_[i].state); }

    void setTip(NOTIFYICONDATAW& nid) const {
        std::string tip = "Desktop Capture";
        for (size_t i = 0; i < status_.size(); ++i) tip += "\n" + statusLine(i);
        const std::wstring w = widen(tip);
        wcsncpy_s(nid.szTip, w.c_str(), _TRUNCATE);
    }

    // Icon, tooltip and the status file after every change.
    void update() {
        if (iconAdded_) {
            NOTIFYICONDATAW nid = iconData();
            nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
            nid.hIcon = currentIcon();
            setTip(nid);
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
        writeFileAtomic(localDataDir() / L"tray-status.txt", formatStatus(quitting_ ? 0 : GetCurrentProcessId(), status_));
    }

    void notify(const std::string& title, const std::string& text, DWORD flags) {
        if (!iconAdded_) return;
        NOTIFYICONDATAW nid = iconData();
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = flags;
        wcsncpy_s(nid.szInfoTitle, widen(title).c_str(), _TRUNCATE);
        wcsncpy_s(nid.szInfo, widen(text).c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void showMenu(int x, int y) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IdOpen, L"ミキサーを開く");
        SetMenuDefaultItem(menu, IdOpen, FALSE);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < status_.size(); ++i)
            AppendMenuW(menu, MF_STRING | MF_GRAYED, IdStatusBase + i, widen(statusLine(i)).c_str());
        AppendMenuW(menu, MF_STRING, IdRestart, L"エンジンとキャプチャを再起動");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const Autostart a = autostartState(exePath());
        const wchar_t* autostartText = a == Autostart::Disabled ? L"ログオン時に起動（Windows の設定でオフ）"
                                       : a == Autostart::Other  ? L"ログオン時に起動（別のフォルダーのもの）"
                                                                : L"ログオン時に起動";
        AppendMenuW(menu, MF_STRING | (a == Autostart::On ? MF_CHECKED : MF_UNCHECKED), IdAutostart, autostartText);
        AppendMenuW(menu, MF_STRING, IdLogs, L"ログのフォルダーを開く");
        AppendMenuW(menu, MF_STRING, IdSettings, L"設定ファイルを開く（tray.txt）");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IdQuit, L"終了（エンジンとキャプチャも止める）");

        SetForegroundWindow(hwnd_);  // so that the menu closes when the user clicks elsewhere
        const UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON |
                           (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN) | TPM_BOTTOMALIGN;
        const UINT cmd = TrackPopupMenuEx(menu, flags, x, y, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu);

        switch (cmd) {
        case IdOpen: openUi(); break;
        case IdRestart: restartAll(); break;
        case IdLogs: openFolder(logDir()); break;
        case IdSettings: openSettings(); break;
        case IdAutostart: {
            std::string err;
            const bool on = a != Autostart::On;
            if (setAutostart(exePath(), on, &err)) logLine(std::string("autostart ") + (on ? "on" : "off"));
            else notify("ログオン時の起動を変えられません", err, NIIF_ERROR);
            break;
        }
        case IdQuit: quit(); break;
        default: break;
        }
    }

    static void openFolder(const fs::path& dir) { ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

    void openSettings() {
        const fs::path file = appDataDir() / L"tray.txt";
        std::error_code ec;
        if (!fs::exists(file, ec)) writeFileAtomic(file, kSettingsTemplate);
        ShellExecuteW(nullptr, L"open", L"notepad.exe", file.c_str(), nullptr, SW_SHOWNORMAL);
    }

    Args args_;
    TrayConfig config_;
    HWND hwnd_ = nullptr;
    UINT taskbarCreated_ = 0;
    bool iconAdded_ = false;
    bool quitting_ = false;
    std::vector<std::unique_ptr<Child>> children_;
    std::vector<ChildStatus> status_;
    std::vector<RestartTracker> trackers_;
    std::vector<std::string> labels_;
    HICON icons_[4] = {};
    fs::path uiExe_;
    HANDLE ui_ = nullptr;
    DWORD uiPid_ = 0;
};

// Hands the request to the tray that already runs. Returns the exit code for this process.
int forwardToRunning(const Args& args) {
    HWND other = FindWindowW(kWindowClass, nullptr);
    if (!other) return 1;  // still starting; the user can click again
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    if (args.quit) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(other, WM_QUIT_TRAY, 0, 0);
        // capd and mixd each get kStopTimeoutMs before they are terminated
        const DWORD r = h ? WaitForSingleObject(h, 2 * kStopTimeoutMs + 15000) : WAIT_FAILED;
        if (h) CloseHandle(h);
        return r == WAIT_OBJECT_0 ? 0 : 1;
    }
    if (!args.background) {
        AllowSetForegroundWindow(pid);  // the mixer window it opens or raises may come to the front
        PostMessageW(other, WM_OPEN_UI, 0, 0);
    }
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    Args args = parseArgs();
    if (!args.error.empty()) {
        printOut("dc-tray: " + args.error + "\n" + kUsage);
        return 2;
    }
    if (args.help) {
        printOut(kUsage);
        return 0;
    }
    if (args.version) {
        printOut("dc-tray " + std::string(dc::kVersionString) + "\n");
        return 0;
    }
    if (!args.autostart.empty()) {
        const fs::path exe = exePath();
        if (args.autostart == "status") {
            printOut(std::string("autostart=") + autostartKey(autostartState(exe)) + "\n");
            return 0;
        }
        if (args.autostart != "on" && args.autostart != "off") {
            printOut("dc-tray: --autostart takes on, off or status\n");
            return 2;
        }
        std::string err;
        if (!setAutostart(exe, args.autostart == "on", &err)) {
            printOut("dc-tray: " + err + "\n");
            return 1;
        }
        printOut(std::string("autostart=") + autostartKey(autostartState(exe)) + "\n");
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const int code = forwardToRunning(args);
        if (mutex) CloseHandle(mutex);
        return code;
    }
    if (args.quit) {  // nothing runs
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    TrayApp app(std::move(args));
    const int code = app.run(inst);
    if (mutex) CloseHandle(mutex);
    return code;
}
