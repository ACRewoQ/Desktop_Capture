#pragma once

// The engine PC's keyboard and MIDI inputs for the hotkey and MIDI bindings (docs/spec/protocol.md
// 5.9). The Windows implementations (control_input_win.cpp) use RegisterHotKey, a low-level keyboard
// hook while learning, and winmm MIDI input; the fakes let the tests drive ControlBindings anywhere.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace dc::mixd {

// Global hotkeys. Callbacks come from the backend's own thread.
class HotkeyBackend {
public:
    struct Callbacks {
        // A registered hotkey went down (repeat = the key is held and Windows repeats it) or up.
        std::function<void(int id, bool press, bool repeat)> onKey;
        // While learning: the combination pressed ("key:ctrl+f9"), or "" when the user cancelled with Esc.
        std::function<void(const std::string& trigger)> onLearned;
    };

    virtual ~HotkeyBackend() = default;
    virtual bool available() const = 0;
    virtual void start(Callbacks callbacks) = 0;
    virtual void stop() = 0;
    // mods: proto::ctl::Mod bits. False when another application holds the combination.
    // repeat: deliver auto-repeat presses (step bindings).
    virtual bool registerKey(int id, std::uint8_t mods, int virtualKey, bool repeat) = 0;
    virtual void unregisterKey(int id) = 0;
    // Learning swallows the next non-modifier key press and reports it through onLearned.
    virtual void setLearning(bool on) = 0;
};

// MIDI inputs, by unique name. The handler runs on a driver thread.
class MidiBackend {
public:
    using Handler = std::function<void(std::uint8_t status, std::uint8_t data1, std::uint8_t data2)>;
    enum class OpenResult { Ok, Busy, Error };

    virtual ~MidiBackend() = default;
    virtual bool available() const = 0;
    // Current inputs; a repeated name gets " #2", " #3" ...
    virtual std::vector<std::string> list() = 0;
    virtual OpenResult open(const std::string& name, Handler handler, std::string* error) = 0;
    virtual void close(const std::string& name) = 0;
};

std::unique_ptr<HotkeyBackend> makeHotkeyBackend(); // a backend that is not available outside Windows
std::unique_ptr<MidiBackend> makeMidiBackend();
// Backends that are never available (mixd --no-hotkeys): every binding shows "unsupported".
std::unique_ptr<HotkeyBackend> makeDisabledHotkeys();
std::unique_ptr<MidiBackend> makeDisabledMidi();

// --- fakes for the tests ---

class FakeHotkeys : public HotkeyBackend {
public:
    bool available() const override { return true; }
    void start(Callbacks callbacks) override { cb_ = std::move(callbacks); }
    void stop() override {}
    bool registerKey(int id, std::uint8_t mods, int virtualKey, bool repeat) override;
    void unregisterKey(int id) override;
    void setLearning(bool on) override { learning = on; }

    // test side
    // combinations "another application" holds
    void setTaken(std::set<std::pair<std::uint8_t, int>> combos);
    std::atomic<bool> learning{false};
    // Presses the combination if registered; returns false when nothing is registered for it.
    bool press(std::uint8_t mods, int virtualKey, bool release = false, bool repeat = false);
    void learn(const std::string& trigger) { cb_.onLearned(trigger); }
    std::size_t registeredCount() const;

private:
    struct Reg {
        std::uint8_t mods;
        int vk;
        bool repeat;
    };
    mutable std::mutex m_;
    std::map<int, Reg> regs_;
    std::set<std::pair<std::uint8_t, int>> taken_;
    Callbacks cb_;
};

class FakeMidi : public MidiBackend {
public:
    bool available() const override { return true; }
    std::vector<std::string> list() override;
    OpenResult open(const std::string& name, Handler handler, std::string* error) override;
    void close(const std::string& name) override;

    // test side
    void setDevices(std::vector<std::string> names);
    void setBusy(std::set<std::string> names); // held by "another application"

    bool isOpen(const std::string& name) const;
    void send(const std::string& name, std::uint8_t status, std::uint8_t d1, std::uint8_t d2);

private:
    mutable std::mutex m_;
    std::vector<std::string> names_;
    std::set<std::string> busy_;
    std::map<std::string, Handler> open_;
};

} // namespace dc::mixd
