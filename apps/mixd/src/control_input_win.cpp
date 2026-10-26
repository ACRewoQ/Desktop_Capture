// Windows keyboard and MIDI input for the hotkey and MIDI bindings: RegisterHotKey on a message-only
// window owned by one thread (which also polls held keys for their release and runs the low-level
// keyboard hook while learning), and winmm MIDI input with a callback function.

#include "control_input.hpp"

#include "dc/protocol/control.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <future>
#include <thread>

namespace dc::mixd {

namespace {

std::string utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

constexpr UINT kMsgRegister = WM_APP + 1;   // wParam: RegisterRequest*
constexpr UINT kMsgUnregister = WM_APP + 2; // wParam: id
constexpr UINT kMsgLearn = WM_APP + 3;      // wParam: 1 = start, 0 = stop
constexpr UINT_PTR kReleaseTimer = 1;
constexpr UINT kReleasePollMs = 15;

struct RegisterRequest {
    int id;
    std::uint8_t mods;
    int vk;
    bool repeat;
};

bool isModifier(DWORD vk) {
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return true;
    default: return false;
    }
}

bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

class WinHotkeys : public HotkeyBackend {
public:
    ~WinHotkeys() override { stop(); }

    bool available() const override { return true; }

    void start(Callbacks callbacks) override {
        cb_ = std::move(callbacks);
        std::promise<HWND> ready;
        auto hwnd = ready.get_future();
        thread_ = std::thread([this, &ready] { run(ready); });
        hwnd_ = hwnd.get();
    }

    void stop() override {
        if (!thread_.joinable()) return;
        if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        thread_.join();
        hwnd_ = nullptr;
    }

    bool registerKey(int id, std::uint8_t mods, int virtualKey, bool repeat) override {
        if (!hwnd_) return false;
        RegisterRequest r{id, mods, virtualKey, repeat};
        return SendMessageW(hwnd_, kMsgRegister, reinterpret_cast<WPARAM>(&r), 0) != 0;
    }

    void unregisterKey(int id) override {
        if (hwnd_) SendMessageW(hwnd_, kMsgUnregister, static_cast<WPARAM>(id), 0);
    }

    void setLearning(bool on) override {
        if (hwnd_) PostMessageW(hwnd_, kMsgLearn, on ? 1 : 0, 0);
    }

private:
    struct Reg {
        int vk = 0;
        bool held = false;
    };

    void run(std::promise<HWND>& ready) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &WinHotkeys::wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DcMixdHotkeys";
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
        ready.set_value(hwnd);
        if (!hwnd) return;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        hook(false);
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<WinHotkeys*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) {
            LRESULT result = 0;
            if (self->handle(hwnd, msg, wp, lp, result)) return result;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    bool handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM, LRESULT& result) {
        switch (msg) {
        case kMsgRegister: {
            const auto& r = *reinterpret_cast<const RegisterRequest*>(wp);
            if (regs_.count(r.id)) {
                UnregisterHotKey(hwnd, r.id);
                regs_.erase(r.id);
            }
            UINT fs = r.repeat ? 0 : MOD_NOREPEAT;
            if (r.mods & proto::ctl::ModCtrl) fs |= MOD_CONTROL;
            if (r.mods & proto::ctl::ModAlt) fs |= MOD_ALT;
            if (r.mods & proto::ctl::ModShift) fs |= MOD_SHIFT;
            if (r.mods & proto::ctl::ModWin) fs |= MOD_WIN;
            const bool ok = RegisterHotKey(hwnd, r.id, fs, static_cast<UINT>(r.vk)) != 0;
            if (ok) regs_[r.id] = {r.vk, false};
            result = ok ? 1 : 0;
            return true;
        }
        case kMsgUnregister: {
            const int id = static_cast<int>(wp);
            if (regs_.erase(id)) UnregisterHotKey(hwnd, id);
            return true;
        }
        case kMsgLearn:
            hook(wp != 0);
            return true;
        case WM_HOTKEY: {
            const int id = static_cast<int>(wp);
            auto it = regs_.find(id);
            if (it == regs_.end()) return true;
            const bool repeat = it->second.held;
            it->second.held = true;
            SetTimer(hwnd, kReleaseTimer, kReleasePollMs, nullptr);
            if (cb_.onKey) cb_.onKey(id, true, repeat);
            return true;
        }
        case WM_TIMER: {
            if (wp != kReleaseTimer) return false;
            bool any = false;
            for (auto& [id, reg] : regs_) {
                if (!reg.held) continue;
                if (down(reg.vk)) {
                    any = true;
                    continue;
                }
                reg.held = false;
                if (cb_.onKey) cb_.onKey(id, false, false);
            }
            if (!any) KillTimer(hwnd, kReleaseTimer);
            return true;
        }
        case WM_CLOSE:
            for (const auto& [id, reg] : regs_) UnregisterHotKey(hwnd, id);
            regs_.clear();
            DestroyWindow(hwnd);
            return true;
        case WM_DESTROY:
            PostQuitMessage(0);
            return true;
        default: return false;
        }
    }

    void hook(bool on) {
        if (on && !hook_) {
            hookOwner_ = this;
            swallowVk_ = 0;
            hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &WinHotkeys::keyboardProc, GetModuleHandleW(nullptr), 0);
        } else if (!on && hook_ && swallowVk_ == 0) { // else the release of the learned key unhooks
            UnhookWindowsHookEx(hook_);
            hook_ = nullptr;
        }
        learning_ = on && hook_;
    }

    // Runs on the thread that installed the hook (this one), between its messages.
    static LRESULT CALLBACK keyboardProc(int code, WPARAM wp, LPARAM lp) {
        WinHotkeys* self = hookOwner_;
        if (code == HC_ACTION && self) {
            const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
            const bool isDown = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
            const bool isUp = wp == WM_KEYUP || wp == WM_SYSKEYUP;
            if (isUp && self->swallowVk_ != 0 && k->vkCode == self->swallowVk_) {
                self->swallowVk_ = 0;
                // unhooking inside the hook procedure is not allowed; the window does it next
                PostMessageW(self->hwnd_, kMsgLearn, 0, 0);
                return 1; // the release of the learned key
            }
            if (isDown && self->learning_ && !isModifier(k->vkCode)) {
                std::uint8_t mods = 0;
                if (down(VK_CONTROL)) mods |= proto::ctl::ModCtrl;
                if (down(VK_MENU)) mods |= proto::ctl::ModAlt;
                if (down(VK_SHIFT)) mods |= proto::ctl::ModShift;
                if (down(VK_LWIN) || down(VK_RWIN)) mods |= proto::ctl::ModWin;
                const std::string name = proto::ctl::keyNameOf(static_cast<int>(k->vkCode));
                if (!name.empty()) {
                    std::string trigger;
                    if (!(k->vkCode == VK_ESCAPE && mods == 0)) {
                        proto::ctl::Trigger t;
                        t.type = proto::ctl::Trigger::Type::Key;
                        t.mods = mods;
                        t.key = name;
                        trigger = proto::ctl::formatTrigger(t);
                    }
                    self->learning_ = false;
                    self->swallowVk_ = k->vkCode;
                    if (self->cb_.onLearned) self->cb_.onLearned(trigger);
                    return 1;
                }
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    Callbacks cb_;
    std::thread thread_;
    std::atomic<HWND> hwnd_{nullptr};
    std::map<int, Reg> regs_; // window thread only
    HHOOK hook_ = nullptr;
    bool learning_ = false;
    DWORD swallowVk_ = 0;
    static inline WinHotkeys* hookOwner_ = nullptr;
};

class WinMidi : public MidiBackend {
public:
    ~WinMidi() override {
        std::lock_guard lock(m_);
        for (auto& [name, o] : open_) closeHandle(*o);
        open_.clear();
    }

    bool available() const override { return true; }

    std::vector<std::string> list() override {
        std::vector<std::string> names;
        const UINT n = midiInGetNumDevs();
        for (UINT i = 0; i < n; ++i) {
            MIDIINCAPSW caps{};
            std::string name;
            if (midiInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR) name = utf8(caps.szPname);
            if (name.empty()) name = "MIDI " + std::to_string(i + 1);
            std::string unique = name;
            for (int k = 2; std::find(names.begin(), names.end(), unique) != names.end(); ++k)
                unique = name + " #" + std::to_string(k);
            names.push_back(unique);
        }
        return names;
    }

    OpenResult open(const std::string& name, Handler handler, std::string* error) override {
        const auto names = list();
        const auto it = std::find(names.begin(), names.end(), name);
        if (it == names.end()) {
            if (error) *error = "見つからない";
            return OpenResult::Error;
        }
        auto o = std::make_unique<Open>();
        o->handler = std::move(handler);
        const UINT index = static_cast<UINT>(it - names.begin());
        const MMRESULT r = midiInOpen(&o->h, index, reinterpret_cast<DWORD_PTR>(&WinMidi::callback),
                                      reinterpret_cast<DWORD_PTR>(o.get()), CALLBACK_FUNCTION);
        if (r != MMSYSERR_NOERROR) {
            if (error) {
                wchar_t text[MAXERRORLENGTH] = {};
                midiInGetErrorTextW(r, text, MAXERRORLENGTH);
                *error = utf8(text);
            }
            return r == MMSYSERR_ALLOCATED ? OpenResult::Busy : OpenResult::Error;
        }
        midiInStart(o->h);
        std::lock_guard lock(m_);
        if (auto old = open_.find(name); old != open_.end()) closeHandle(*old->second);
        open_[name] = std::move(o);
        return OpenResult::Ok;
    }

    void close(const std::string& name) override {
        std::unique_ptr<Open> o;
        {
            std::lock_guard lock(m_);
            auto it = open_.find(name);
            if (it == open_.end()) return;
            o = std::move(it->second);
            open_.erase(it);
        }
        closeHandle(*o);
    }

private:
    struct Open {
        HMIDIIN h = nullptr;
        Handler handler;
    };

    static void closeHandle(Open& o) {
        if (!o.h) return;
        midiInStop(o.h);
        midiInReset(o.h);
        midiInClose(o.h); // no callbacks after this returns
        o.h = nullptr;
    }

    static void CALLBACK callback(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR) {
        if (msg != MIM_DATA) return;
        auto* o = reinterpret_cast<Open*>(instance);
        const auto v = static_cast<std::uint32_t>(p1);
        if (o->handler) o->handler(static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>((v >> 8) & 0xFF),
                                   static_cast<std::uint8_t>((v >> 16) & 0xFF));
    }

    std::mutex m_;
    std::map<std::string, std::unique_ptr<Open>> open_;
};

} // namespace

std::unique_ptr<HotkeyBackend> makeHotkeyBackend() { return std::make_unique<WinHotkeys>(); }
std::unique_ptr<MidiBackend> makeMidiBackend() { return std::make_unique<WinMidi>(); }

} // namespace dc::mixd
