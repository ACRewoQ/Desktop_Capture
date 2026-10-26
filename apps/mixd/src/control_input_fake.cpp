#include "control_input.hpp"

#include <algorithm>

namespace dc::mixd {

bool FakeHotkeys::registerKey(int id, std::uint8_t mods, int virtualKey, bool repeat) {
    std::lock_guard lock(m_);
    if (taken_.count({mods, virtualKey})) return false;
    regs_[id] = {mods, virtualKey, repeat};
    return true;
}

void FakeHotkeys::setTaken(std::set<std::pair<std::uint8_t, int>> combos) {
    std::lock_guard lock(m_);
    taken_ = std::move(combos);
}

void FakeHotkeys::unregisterKey(int id) {
    std::lock_guard lock(m_);
    regs_.erase(id);
}

bool FakeHotkeys::press(std::uint8_t mods, int virtualKey, bool release, bool repeat) {
    int id = -1;
    {
        std::lock_guard lock(m_);
        for (const auto& [i, r] : regs_)
            if (r.mods == mods && r.vk == virtualKey && (!repeat || r.repeat)) id = i;
    }
    if (id < 0) return false;
    cb_.onKey(id, !release, repeat);
    return true;
}

std::size_t FakeHotkeys::registeredCount() const {
    std::lock_guard lock(m_);
    return regs_.size();
}

std::vector<std::string> FakeMidi::list() {
    std::lock_guard lock(m_);
    return names_;
}

MidiBackend::OpenResult FakeMidi::open(const std::string& name, Handler handler, std::string* error) {
    std::lock_guard lock(m_);
    if (std::find(names_.begin(), names_.end(), name) == names_.end()) {
        if (error) *error = "no such device";
        return OpenResult::Error;
    }
    if (busy_.count(name)) {
        if (error) *error = "in use";
        return OpenResult::Busy;
    }
    open_[name] = std::move(handler);
    return OpenResult::Ok;
}

void FakeMidi::close(const std::string& name) {
    std::lock_guard lock(m_);
    open_.erase(name);
}

void FakeMidi::setDevices(std::vector<std::string> names) {
    std::lock_guard lock(m_);
    names_ = std::move(names);
}

void FakeMidi::setBusy(std::set<std::string> names) {
    std::lock_guard lock(m_);
    busy_ = std::move(names);
}

bool FakeMidi::isOpen(const std::string& name) const {
    std::lock_guard lock(m_);
    return open_.count(name) > 0;
}

void FakeMidi::send(const std::string& name, std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
    Handler h;
    {
        std::lock_guard lock(m_);
        if (auto it = open_.find(name); it != open_.end()) h = it->second;
    }
    if (h) h(status, d1, d2);
}

namespace {
class NoHotkeys : public HotkeyBackend {
public:
    bool available() const override { return false; }
    void start(Callbacks) override {}
    void stop() override {}
    bool registerKey(int, std::uint8_t, int, bool) override { return false; }
    void unregisterKey(int) override {}
    void setLearning(bool) override {}
};
class NoMidi : public MidiBackend {
public:
    bool available() const override { return false; }
    std::vector<std::string> list() override { return {}; }
    OpenResult open(const std::string&, Handler, std::string* error) override {
        if (error) *error = "MIDI input is off";
        return OpenResult::Error;
    }
    void close(const std::string&) override {}
};
} // namespace

std::unique_ptr<HotkeyBackend> makeDisabledHotkeys() { return std::make_unique<NoHotkeys>(); }
std::unique_ptr<MidiBackend> makeDisabledMidi() { return std::make_unique<NoMidi>(); }

#ifndef _WIN32
std::unique_ptr<HotkeyBackend> makeHotkeyBackend() { return makeDisabledHotkeys(); }
std::unique_ptr<MidiBackend> makeMidiBackend() { return makeDisabledMidi(); }
#endif

} // namespace dc::mixd
