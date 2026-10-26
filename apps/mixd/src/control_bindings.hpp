#pragma once

// Hotkeys and MIDI bindings (docs/spec/protocol.md 5.9), on the network thread: keeps
// /control/binding/# checked (state, error), registers the hotkeys, opens the enabled MIDI inputs
// and lists them under /control/midi/device/#, runs the bound actions through the same checks as a
// client's Set or Call, learns triggers, and saves the bindings to a small text file.

#include "control_input.hpp"

#include "dc/control/server.hpp"
#include "dc/protocol/control.hpp"

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace dc::mixd {

class ControlBindings {
public:
    struct Options {
        std::string storePath; // "" = keep nothing (tests)
        std::chrono::milliseconds midiRescan{3000};
        std::chrono::milliseconds learnTimeout{30000};
        std::chrono::milliseconds paramInterval{16}; // param bindings: ~60 Hz
        std::chrono::milliseconds lastInterval{100}; // /control/midi/last: ~10 Hz
        bool verbose = true;
    };

    ControlBindings(asio::io_context& io, control::ControlServer& server, HotkeyBackend& keys, MidiBackend& midi,
                    Options options);
    ~ControlBindings();

    // Loads the saved bindings into the tree, registers the methods, starts the inputs. Network thread.
    void start();
    void stop();

    // ControlServer::Options::onDelta (network thread).
    void onDelta(const proto::Delta& delta);
    // Looks for MIDI inputs now (/devices/refresh).
    void refreshMidi();

    // %APPDATA%\DesktopCapture\mixd-control.txt, ~/.config/desktop-capture/mixd-control.txt
    static std::string defaultPath();

private:
    struct Hotkey {
        int id = 0;
        bool registered = false;
        bool repeat = false;
    };
    struct MidiDevice {
        std::string state; // ok / off / busy / error
        bool open = false;
    };
    struct PendingParam {
        std::string path;
        double normalized = 0;
    };

    proto::Result add(control::CallContext& ctx);
    proto::Result remove(control::CallContext& ctx);
    proto::Result learn(control::CallContext& ctx);
    void endLearn();
    void learned(const std::string& trigger, const std::string& device);

    void scheduleCheck();
    void check();
    std::vector<int> bindingNumbers() const;

    void onHotkey(int id, bool press, bool repeat);
    void onMidi(const std::string& device, std::uint8_t status, std::uint8_t d1, std::uint8_t d2);
    void fire(int binding, const proto::ctl::Binding& b, bool press, bool repeat, std::optional<double> normalized);
    void flushParams();
    void publishLast();
    void scheduleRescan();
    void publishMidiDevices();
    void openMidi(const std::string& name);

    void scheduleSave();
    void load();
    void save() const;

    asio::io_context& io_;
    control::ControlServer& server_;
    HotkeyBackend& keys_;
    MidiBackend& midi_;
    Options options_;
    control::ClientInfo self_;
    bool stopped_ = false;
    bool checkPending_ = false;

    struct Bound {
        int number = 0;
        proto::ctl::Binding binding;
    };
    std::map<std::string, std::vector<Bound>> byTrigger_; // bindings in state ok, by trigger
    std::map<std::string, Hotkey> hotkeys_; // by trigger
    bool retryHotkeys_ = false;
    int nextHotkeyId_ = 1;
    std::map<int, std::string> hotkeyTrigger_; // id -> trigger

    std::vector<std::string> midiNames_;       // as listed, in order
    std::map<std::string, MidiDevice> midiDevices_;
    std::set<std::string> midiDisabled_;       // by name, saved
    std::map<std::string, bool> midiHeld_;      // "<device>|<trigger>" -> down (button edges)
    asio::steady_timer rescanTimer_;

    int learning_ = 0;
    std::string learnSource_;
    asio::steady_timer learnTimer_;

    std::map<int, PendingParam> pendingParams_;
    asio::steady_timer paramTimer_;
    bool paramTimerRunning_ = false;
    std::optional<std::tuple<std::string, std::string, int>> pendingLast_; // trigger, device, value
    asio::steady_timer lastTimer_;
    bool lastTimerRunning_ = false;

    asio::steady_timer saveTimer_;
    bool savePending_ = false;
    bool loading_ = false;
};

} // namespace dc::mixd
