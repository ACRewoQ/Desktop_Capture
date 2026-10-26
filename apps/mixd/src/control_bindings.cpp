#include "control_bindings.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace dc::mixd {

using namespace dc::proto;
using control::CallContext;

namespace {

constexpr const char* kBase = "/control/binding/";

std::string base(int n) { return kBase + std::to_string(n); }

Result error(ErrorCode code, std::string message) {
    Result r;
    r.code = code;
    r.message = std::move(message);
    return r;
}

std::optional<std::int64_t> intArg(const Args& args, const char* key) {
    if (auto it = args.find(key); it != args.end())
        if (auto* v = std::get_if<std::int64_t>(&it->second)) return *v;
    return std::nullopt;
}

std::optional<std::string> strArg(const Args& args, const char* key) {
    if (auto it = args.find(key); it != args.end())
        if (auto* v = std::get_if<std::string>(&it->second)) return *v;
    return std::nullopt;
}

bool endsWith(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && std::string_view(s).substr(s.size() - suffix.size()) == suffix;
}

// "/control/binding/12/trigger" -> 12, 0 otherwise
int bindingOf(const std::string& path) {
    if (path.rfind(kBase, 0) != 0) return 0;
    const auto start = std::strlen(kBase);
    const auto end = path.find('/', start);
    return std::atoi(path.substr(start, end - start).c_str());
}

// Paths whose changes cannot make a binding valid or invalid (engine reports, our own outputs).
bool noise(const std::string& p) {
    if (p.rfind("/control/midi/last/", 0) == 0 || p.rfind("/control/learn/", 0) == 0) return true;
    if (p.rfind("/control/midi/device/", 0) == 0) return !endsWith(p, "/enabled");
    if (p.rfind(kBase, 0) == 0) return endsWith(p, "/state") || endsWith(p, "/error");
    static const char* const kSuffixes[] = {"/elapsed_ms", "/playing", "/cpu_load", "/xruns", "/latency_ms",
                                            "/lan_delay_ms", "/state", "/error", "/progress", "/current"};
    for (const char* s : kSuffixes)
        if (endsWith(p, s)) return true;
    return p.rfind("/lan/", 0) == 0 || p.rfind("/remote/", 0) == 0 || p.rfind("/devices/", 0) == 0;
}

// a change of what is saved
bool saved(const std::string& p) {
    if (p.rfind(kBase, 0) == 0) return !endsWith(p, "/state") && !endsWith(p, "/error");
    return p.rfind("/control/midi/device/", 0) == 0 && endsWith(p, "/enabled");
}

std::filesystem::path fsPath(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string clean(std::string s) {
    for (auto& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

} // namespace

ControlBindings::ControlBindings(asio::io_context& io, control::ControlServer& server, HotkeyBackend& keys,
                                 MidiBackend& midi, Options options)
    : io_(io), server_(server), keys_(keys), midi_(midi), options_(std::move(options)), rescanTimer_(io),
      learnTimer_(io), paramTimer_(io), lastTimer_(io), saveTimer_(io) {
    self_.name = "control";
    self_.role = ClientRole::Local;
    self_.address = "local";
}

ControlBindings::~ControlBindings() {
    keys_.stop();
    for (const auto& [name, d] : midiDevices_)
        if (d.open) midi_.close(name);
}

std::string ControlBindings::defaultPath() {
#ifdef _WIN32
    if (const wchar_t* a = _wgetenv(L"APPDATA")) {
        const auto u8 = (std::filesystem::path(a) / L"DesktopCapture" / L"mixd-control.txt").u8string();
        return std::string(u8.begin(), u8.end());
    }
    return "mixd-control.txt";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/desktop-capture/mixd-control.txt";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/.config/desktop-capture/mixd-control.txt";
    return "mixd-control.txt";
#endif
}

void ControlBindings::start() {
    server_.registerMethod("/control/binding/add", [this](CallContext& ctx) { return add(ctx); });
    server_.registerMethod("/control/binding/remove", [this](CallContext& ctx) { return remove(ctx); });
    server_.registerMethod("/control/learn", [this](CallContext& ctx) { return learn(ctx); });
    server_.registerMethod("/control/learn/cancel", [this](CallContext&) {
        endLearn();
        return Result{};
    });
    keys_.start({[this](int id, bool press, bool repeat) {
                     asio::post(io_, [this, id, press, repeat] {
                         if (!stopped_) onHotkey(id, press, repeat);
                     });
                 },
                 [this](const std::string& trigger) {
                     asio::post(io_, [this, trigger] {
                         if (!stopped_ && learning_ && learnSource_ != "midi") learned(trigger, {});
                     });
                 }});
    load();
    refreshMidi();
    check();
    scheduleRescan();
}

void ControlBindings::stop() {
    stopped_ = true;
    rescanTimer_.cancel();
    learnTimer_.cancel();
    paramTimer_.cancel();
    lastTimer_.cancel();
    if (savePending_) {
        saveTimer_.cancel();
        save();
        savePending_ = false;
    }
    keys_.stop();
    for (auto& [name, d] : midiDevices_)
        if (d.open) {
            midi_.close(name);
            d.open = false;
        }
}

// --- methods ---

Result ControlBindings::add(CallContext& ctx) {
    const auto& tree = server_.store().tree();
    int n = 1;
    while (n <= kMaxBindings && tree.contains(base(n) + "/action")) ++n;
    if (n > kMaxBindings) return error(ErrorCode::LimitReached, "128 bindings already");
    std::vector<Change> c;
    const auto& schema = server_.store().schema();
    for (const char* key : {"trigger", "device", "action", "target", "value"}) {
        const std::string path = base(n) + "/" + key;
        Value v = strArg(ctx.call.args, key).value_or(key == std::string("action") ? "none" : "");
        if (auto e = schema.validate(*schema.find(path), v, true)) {
            e.message = std::string(key) + ": " + e.message;
            return error(e.code, e.message);
        }
        c.push_back({path, std::move(v)});
    }
    c.push_back({base(n) + "/enabled", true});
    c.push_back({base(n) + "/state", std::string("off")});
    c.push_back({base(n) + "/error", std::string()});
    server_.publishNow(c, {}, ctx.call.id, ctx.client.name);
    check(); // the state goes out before the Result
    Result r;
    r.value = std::int64_t{n};
    return r;
}

Result ControlBindings::remove(CallContext& ctx) {
    const auto n = intArg(ctx.call.args, "binding");
    if (!n || !server_.store().tree().contains(base(static_cast<int>(*n)) + "/action"))
        return error(ErrorCode::NotFound, "no such binding");
    if (learning_ == *n) endLearn();
    server_.publishNow({}, {base(static_cast<int>(*n))}, ctx.call.id, ctx.client.name);
    check();
    return {};
}

Result ControlBindings::learn(CallContext& ctx) {
    const auto n = intArg(ctx.call.args, "binding");
    if (!n || !server_.store().tree().contains(base(static_cast<int>(*n)) + "/action"))
        return error(ErrorCode::NotFound, "no such binding");
    const std::string source = strArg(ctx.call.args, "source").value_or("any");
    if (source != "any" && source != "key" && source != "midi") return error(ErrorCode::OutOfRange, "source must be any, key or midi");
    if (source == "key" && !keys_.available()) return error(ErrorCode::Conflict, "hotkeys are not available on this PC");
    if (source == "midi" && !midi_.available()) return error(ErrorCode::Conflict, "MIDI is not available on this PC");
    learning_ = static_cast<int>(*n);
    learnSource_ = source;
    keys_.setLearning(source != "midi");
    server_.publishNow({{"/control/learn/binding", std::int64_t{learning_}}, {"/control/learn/source", source}}, {},
                       ctx.call.id, ctx.client.name);
    learnTimer_.expires_after(options_.learnTimeout);
    learnTimer_.async_wait([this, n = learning_](const asio::error_code& ec) {
        if (!ec && !stopped_ && learning_ == n) endLearn();
    });
    return {};
}

void ControlBindings::endLearn() {
    if (!learning_) return;
    learning_ = 0;
    learnTimer_.cancel();
    keys_.setLearning(false);
    server_.publishNow({{"/control/learn/binding", std::int64_t{0}}});
}

void ControlBindings::learned(const std::string& trigger, const std::string& device) {
    const int n = learning_;
    if (!trigger.empty() && server_.store().tree().contains(base(n) + "/action")) {
        server_.publishNow({{base(n) + "/trigger", trigger}, {base(n) + "/device", device}});
        Event ev;
        ev.name = "control.learned";
        ev.data = {{"binding", std::int64_t{n}}, {"trigger", trigger}, {"device", device}};
        server_.broadcastEventNow(ev);
        if (options_.verbose) std::cout << "control: binding " << n << " learned " << trigger
                                        << (device.empty() ? "" : " from " + device) << std::endl;
    }
    endLearn();
    check();
}

// --- tree follow and checks ---

void ControlBindings::onDelta(const Delta& delta) {
    bool relevant = !delta.removed.empty();
    bool dirty = false;
    for (const auto& r : delta.removed) dirty = dirty || r.rfind(kBase, 0) == 0;
    for (const auto& c : delta.changes) {
        relevant = relevant || !noise(c.path);
        dirty = dirty || saved(c.path);
        if (c.path.rfind("/control/midi/device/", 0) == 0 && endsWith(c.path, "/enabled")) {
            const int i = std::atoi(c.path.substr(21).c_str());
            const auto* on = std::get_if<bool>(&c.value);
            if (on && i >= 1 && i <= static_cast<int>(midiNames_.size())) {
                asio::post(io_, [this, name = midiNames_[static_cast<std::size_t>(i - 1)], enabled = *on] {
                    if (stopped_ || enabled == !midiDisabled_.count(name)) return;
                    if (enabled) midiDisabled_.erase(name);
                    else midiDisabled_.insert(name);
                    auto& d = midiDevices_[name];
                    if (!enabled && d.open) {
                        midi_.close(name);
                        d.open = false;
                    }
                    if (enabled) openMidi(name);
                    else d.state = "off";
                    publishMidiDevices();
                });
            }
        }
    }
    if (dirty && !loading_) scheduleSave();
    if (relevant) scheduleCheck();
}

void ControlBindings::scheduleCheck() {
    if (checkPending_) return;
    checkPending_ = true;
    asio::post(io_, [this] {
        checkPending_ = false;
        if (!stopped_) check();
    });
}

std::vector<int> ControlBindings::bindingNumbers() const {
    std::vector<int> out;
    const auto& values = server_.store().tree().values();
    for (auto it = values.lower_bound(kBase); it != values.end() && it->first.rfind(kBase, 0) == 0; ++it)
        if (endsWith(it->first, "/action")) out.push_back(bindingOf(it->first));
    std::sort(out.begin(), out.end());
    return out;
}

void ControlBindings::check() {
    const auto& tree = server_.store().tree();
    struct Row {
        ctl::Binding b;
        ctl::BindingCheck c;
        std::optional<ctl::Trigger> t;
    };
    std::map<int, Row> rows;
    for (int n : bindingNumbers()) {
        Row r;
        r.b = ctl::readBinding(tree, n);
        r.c = ctl::checkBinding(tree, r.b, server_.store().schema());
        if (r.c.state == "ok") {
            r.t = ctl::parseTrigger(r.b.trigger);
            const bool key = r.t && r.t->type == ctl::Trigger::Type::Key;
            if (key && !keys_.available()) r.c = {"unsupported", "この PC ではホットキーを使えない"};
            else if (!key && !midi_.available()) r.c = {"unsupported", "この PC では MIDI を使えない"};
        }
        rows.emplace(n, std::move(r));
    }

    // hotkeys: one registration per combination; a step binding asks for the auto-repeat
    std::map<std::string, bool> wanted; // trigger -> repeat
    for (const auto& [n, r] : rows)
        if (r.c.state == "ok" && r.t && r.t->type == ctl::Trigger::Type::Key)
            wanted[r.b.trigger] = wanted[r.b.trigger] || r.b.action == "step";
    for (auto it = hotkeys_.begin(); it != hotkeys_.end();) {
        if (wanted.count(it->first)) {
            ++it;
            continue;
        }
        if (it->second.registered) keys_.unregisterKey(it->second.id);
        hotkeyTrigger_.erase(it->second.id);
        it = hotkeys_.erase(it);
    }
    for (const auto& [trigger, repeat] : wanted) {
        auto [it, fresh] = hotkeys_.try_emplace(trigger);
        Hotkey& h = it->second;
        if (fresh) {
            h.id = nextHotkeyId_++;
            if (nextHotkeyId_ > 0xBFFF) nextHotkeyId_ = 1;
            hotkeyTrigger_[h.id] = trigger;
        }
        if (fresh || (!h.registered && retryHotkeys_) || (h.registered && h.repeat != repeat)) {
            const auto t = ctl::parseTrigger(trigger);
            h.registered = keys_.registerKey(h.id, t->mods, ctl::virtualKeyOf(t->key), repeat);
            h.repeat = repeat;
            if (options_.verbose && !h.registered) std::cout << "control: hotkey " << trigger << " is taken by another application" << std::endl;
        }
    }
    retryHotkeys_ = false;

    std::vector<Change> c;
    byTrigger_.clear();
    for (auto& [n, r] : rows) {
        if (r.c.state == "ok" && r.t && r.t->type == ctl::Trigger::Type::Key && !hotkeys_[r.b.trigger].registered)
            r.c = {"conflict", "他のアプリが同じキーの組み合わせを使用中"};
        if (r.c.state == "ok") byTrigger_[r.b.trigger].push_back({n, r.b});
        if (tree.getAs<std::string>(base(n) + "/state") != r.c.state) c.push_back({base(n) + "/state", r.c.state});
        if (tree.getAs<std::string>(base(n) + "/error") != r.c.error) c.push_back({base(n) + "/error", r.c.error});
    }
    if (!c.empty()) server_.publishNow(c);
}

// --- inputs ---

void ControlBindings::onHotkey(int id, bool press, bool repeat) {
    const auto t = hotkeyTrigger_.find(id);
    if (t == hotkeyTrigger_.end()) return;
    const auto it = byTrigger_.find(t->second);
    if (it == byTrigger_.end()) return;
    const auto bound = it->second; // actions may change the tree and so byTrigger_
    for (const auto& b : bound) {
        if (repeat && b.binding.action != "step") continue;
        fire(b.number, b.binding, press, repeat, std::nullopt);
    }
}

void ControlBindings::onMidi(const std::string& device, std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
    const auto m = ctl::decodeMidi(status, d1, d2);
    if (!m) return;
    const std::string trigger = ctl::formatTrigger(ctl::triggerOf(*m));

    pendingLast_ = std::make_tuple(trigger, device, m->value);
    if (!lastTimerRunning_) publishLast();

    if (learning_ && learnSource_ != "key") {
        if (m->press || m->kind == ctl::MidiKind::Cc || m->kind == ctl::MidiKind::Pb) learned(trigger, device);
        return;
    }

    // buttons act on edges: a pad sending 127 twice presses once
    bool press = m->press, release = m->release;
    if (m->kind == ctl::MidiKind::Note || m->kind == ctl::MidiKind::Cc) {
        bool& held = midiHeld_[device + "|" + trigger];
        press = press && !held;
        release = release && held;
        if (press) held = true;
        if (release) held = false;
    }

    const auto it = byTrigger_.find(trigger);
    if (it == byTrigger_.end()) return;
    const auto bound = it->second;
    for (const auto& b : bound) {
        if (!b.binding.device.empty() && b.binding.device != device) continue;
        if (b.binding.action == "param") {
            if (m->kind == ctl::MidiKind::Note && !press && !release) continue;
            fire(b.number, b.binding, true, false, ctl::normalizedValue(*m));
        } else if (press || release) {
            fire(b.number, b.binding, press, false, std::nullopt);
        }
    }
}

void ControlBindings::fire(int binding, const ctl::Binding& b, bool press, bool repeat,
                           std::optional<double> normalized) {
    const auto& tree = server_.store().tree();
    auto call = [&](const std::string& method, Args args = {}) {
        Call c;
        c.method = method;
        c.args = std::move(args);
        const Result r = server_.callLocal(c, self_);
        if (options_.verbose && r.code != ErrorCode::Ok)
            std::cout << "control: binding " << binding << " " << method << ": " << r.message << std::endl;
    };
    auto set = [&](const std::string& path, std::optional<Value> v) {
        if (!v) return;
        Set s;
        s.changes = {{path, std::move(*v)}};
        const Result r = server_.applyLocalSet(s, self_.name);
        if (options_.verbose && r.code != ErrorCode::Ok)
            std::cout << "control: binding " << binding << " " << path << ": " << r.message << std::endl;
    };
    const std::string& a = b.action;
    if (options_.verbose && press && !repeat && a != "param")
        std::cout << "control: " << b.trigger << " -> " << a << (b.target.empty() ? "" : " " + b.target) << std::endl;

    if (a == "param") {
        if (!normalized) return;
        if (paramTimerRunning_) {
            pendingParams_[binding] = {b.target, *normalized};
            return;
        }
        set(b.target, ctl::valueFromMidi(tree, b.target, *normalized));
        paramTimerRunning_ = true;
        paramTimer_.expires_after(options_.paramInterval);
        paramTimer_.async_wait([this](const asio::error_code& ec) {
            if (!ec && !stopped_) flushParams();
        });
        return;
    }
    if (a == "pad") {
        const auto pad = ctl::parsePad(b.target);
        Args args{{"pad", std::int64_t{pad->pad}}};
        if (pad->bank) args["bank"] = std::int64_t{pad->bank};
        call(press ? "/sampler/trigger" : "/sampler/release", std::move(args));
        return;
    }
    if (!press) return;
    if (a == "record_toggle") {
        const auto state = tree.getAs<std::string>("/capture/state").value_or("idle");
        call(state == "recording" || state == "starting" ? "/capture/record/stop" : "/capture/record/start");
    } else if (a == "record_start") {
        call("/capture/record/start");
    } else if (a == "record_stop") {
        call("/capture/record/stop");
    } else if (a == "recorder_toggle") {
        call(tree.getAs<std::string>("/recorder/state") == "recording" ? "/recorder/stop" : "/recorder/start");
    } else if (a == "pad_stop") {
        Args args;
        if (!b.target.empty()) args["bank"] = std::int64_t{std::atoi(b.target.c_str())};
        call("/sampler/stop", std::move(args));
    } else if (a == "take") {
        call("/video/mix/" + b.target + "/take");
    } else if (a == "toggle") {
        set(b.target, Value{!tree.getAs<bool>(b.target).value_or(false)});
    } else if (a == "set") {
        set(b.target, ctl::parseValue(tree, b.target, b.value, server_.store().schema()));
    } else if (a == "step") {
        set(b.target, ctl::stepValue(tree, b.target, std::strtod(b.value.c_str(), nullptr), server_.store().schema()));
    }
}

void ControlBindings::flushParams() {
    if (pendingParams_.empty()) {
        paramTimerRunning_ = false;
        return;
    }
    const auto& tree = server_.store().tree();
    Set s;
    for (const auto& [n, p] : pendingParams_)
        if (auto v = ctl::valueFromMidi(tree, p.path, p.normalized, server_.store().schema()))
            s.changes.push_back({p.path, std::move(*v)});
    pendingParams_.clear();
    // several bindings may move the same parameter: the last one wins, and one failing must not block the others
    for (auto& ch : s.changes) {
        Set one;
        one.changes = {ch};
        server_.applyLocalSet(one, self_.name);
    }
    paramTimer_.expires_after(options_.paramInterval);
    paramTimer_.async_wait([this](const asio::error_code& ec) {
        if (!ec && !stopped_) flushParams();
    });
}

void ControlBindings::publishLast() {
    if (!pendingLast_) {
        lastTimerRunning_ = false;
        return;
    }
    const auto& [trigger, device, value] = *pendingLast_;
    server_.publishNow({{"/control/midi/last/trigger", trigger},
                        {"/control/midi/last/device", device},
                        {"/control/midi/last/value", std::int64_t{value}}});
    pendingLast_.reset();
    lastTimerRunning_ = true;
    lastTimer_.expires_after(options_.lastInterval);
    lastTimer_.async_wait([this](const asio::error_code& ec) {
        if (!ec && !stopped_) publishLast();
    });
}

// --- MIDI devices ---

void ControlBindings::scheduleRescan() {
    rescanTimer_.expires_after(options_.midiRescan);
    rescanTimer_.async_wait([this](const asio::error_code& ec) {
        if (ec || stopped_) return;
        refreshMidi();
        retryHotkeys_ = true; // another application may have let a combination go
        check();
        scheduleRescan();
    });
}

void ControlBindings::refreshMidi() {
    if (!midi_.available()) return;
    auto names = midi_.list();
    if (names.size() > static_cast<std::size_t>(kMaxMidiDevices)) names.resize(kMaxMidiDevices);
    for (auto it = midiDevices_.begin(); it != midiDevices_.end();) {
        if (std::find(names.begin(), names.end(), it->first) != names.end()) {
            ++it;
            continue;
        }
        if (it->second.open) midi_.close(it->first);
        if (options_.verbose) std::cout << "control: MIDI input gone: " << it->first << std::endl;
        it = midiDevices_.erase(it);
    }
    for (const auto& name : names) {
        auto& d = midiDevices_[name];
        if (midiDisabled_.count(name)) d.state = "off";
        else if (!d.open) openMidi(name);
    }
    midiNames_ = std::move(names);
    publishMidiDevices(); // states may have changed too
}

void ControlBindings::openMidi(const std::string& name) {
    auto& d = midiDevices_[name];
    if (d.open) return;
    std::string why;
    const auto r = midi_.open(
        name,
        [this, name](std::uint8_t s, std::uint8_t a, std::uint8_t b) {
            asio::post(io_, [this, name, s, a, b] {
                if (!stopped_) onMidi(name, s, a, b);
            });
        },
        &why);
    const std::string before = d.state;
    d.open = r == MidiBackend::OpenResult::Ok;
    d.state = d.open ? "ok" : r == MidiBackend::OpenResult::Busy ? "busy" : "error";
    if (options_.verbose && d.state != before)
        std::cout << "control: MIDI input " << name << ": " << d.state << (why.empty() ? "" : " (" + why + ")") << std::endl;
}

void ControlBindings::publishMidiDevices() {
    std::vector<Change> c;
    for (std::size_t i = 0; i < midiNames_.size(); ++i) {
        const std::string b = "/control/midi/device/" + std::to_string(i + 1);
        const auto& name = midiNames_[i];
        c.push_back({b + "/name", name});
        c.push_back({b + "/enabled", !midiDisabled_.count(name)});
        c.push_back({b + "/state", midiDevices_[name].state});
    }
    std::vector<std::string> removed;
    const auto& tree = server_.store().tree();
    for (int i = static_cast<int>(midiNames_.size()) + 1; i <= kMaxMidiDevices; ++i)
        if (tree.contains("/control/midi/device/" + std::to_string(i) + "/name"))
            removed.push_back("/control/midi/device/" + std::to_string(i));
    server_.publishNow(c, removed);
}

// --- saving ---

void ControlBindings::scheduleSave() {
    if (options_.storePath.empty() || savePending_) return;
    savePending_ = true;
    saveTimer_.expires_after(std::chrono::seconds(1));
    saveTimer_.async_wait([this](const asio::error_code& ec) {
        if (ec) return;
        savePending_ = false;
        save();
    });
}

void ControlBindings::load() {
    if (options_.storePath.empty()) return;
    std::ifstream in(fsPath(options_.storePath));
    if (!in) return;
    std::vector<Change> c;
    const auto& schema = server_.store().schema();
    std::string line;
    while (std::getline(in, line)) {
        const auto f = splitTabs(line);
        if (f[0] == "midi_disabled" && f.size() >= 2) {
            midiDisabled_.insert(f[1]);
        } else if (f[0] == "binding" && f.size() >= 8) {
            const int n = std::atoi(f[1].c_str());
            if (n < 1 || n > kMaxBindings) continue;
            std::vector<Change> one = {{base(n) + "/enabled", f[2] == "1"}, {base(n) + "/trigger", f[3]},
                                       {base(n) + "/device", f[4]},      {base(n) + "/action", f[5]},
                                       {base(n) + "/target", f[6]},      {base(n) + "/value", f[7]}};
            bool ok = true;
            for (const auto& x : one) ok = ok && !schema.validate(*schema.find(x.path), x.value, true);
            if (!ok) continue; // written by a newer engine, or edited by hand
            one.push_back({base(n) + "/state", std::string("off")});
            one.push_back({base(n) + "/error", std::string()});
            c.insert(c.end(), one.begin(), one.end());
        }
    }
    loading_ = true;
    if (!c.empty()) server_.publishNow(c);
    loading_ = false;
    if (options_.verbose && !c.empty()) std::cout << "control: loaded bindings from " << options_.storePath << std::endl;
}

void ControlBindings::save() const {
    if (options_.storePath.empty()) return;
    const auto& tree = server_.store().tree();
    std::error_code ec;
    const auto path = fsPath(options_.storePath);
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "# Desktop Capture mixd: hotkey and MIDI bindings (docs/spec/protocol.md 5.9)\n";
        for (int n : bindingNumbers()) {
            const auto b = ctl::readBinding(tree, n);
            out << "binding\t" << n << "\t" << (b.enabled ? "1" : "0") << "\t" << clean(b.trigger) << "\t"
                << clean(b.device) << "\t" << clean(b.action) << "\t" << clean(b.target) << "\t" << clean(b.value) << "\n";
        }
        for (const auto& name : midiDisabled_) out << "midi_disabled\t" << clean(name) << "\n";
        if (!out) return;
    }
    std::filesystem::rename(tmp, path, ec);
}

} // namespace dc::mixd
