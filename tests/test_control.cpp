// Hotkeys and MIDI bindings (protocol.md 5.9): the shared helpers, and the engine's ControlBindings with
// fake keyboard and MIDI inputs behind the real control server.

#include "control_bindings.hpp"
#include "controller.hpp"
#include "device_manager.hpp"
#include "engine.hpp"
#include "fake_backend.hpp"

#include "dc/control/client.hpp"
#include "dc/protocol/control.hpp"
#include "dc/protocol/defaults.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace dc;
using namespace dc::mixd;
using namespace std::chrono_literals;
using Catch::Approx;
namespace ctl = dc::proto::ctl;

namespace {

proto::ParamTree defaultTree() {
    proto::ParamTree t;
    for (auto& c : proto::globalDefaults()) t.set(c.path, c.value);
    for (auto& c : proto::masterDefaults()) t.set(c.path, c.value);
    for (auto& c : proto::trackDefaults(1)) t.set(c.path, c.value);
    t.set("/track/1/position", std::int64_t{1});
    return t;
}

ctl::Binding bind(std::string trigger, std::string action, std::string target = {}, std::string value = {}) {
    ctl::Binding b;
    b.trigger = std::move(trigger);
    b.action = std::move(action);
    b.target = std::move(target);
    b.value = std::move(value);
    return b;
}

std::string stateOf(const proto::ParamTree& t, const ctl::Binding& b) { return ctl::checkBinding(t, b).state; }

} // namespace

TEST_CASE("control: triggers parse, print and name their keys", "[control]") {
    const auto k = ctl::parseTrigger("key:ctrl+shift+f9");
    REQUIRE(k);
    CHECK(k->type == ctl::Trigger::Type::Key);
    CHECK(k->mods == (ctl::ModCtrl | ctl::ModShift));
    CHECK(k->key == "f9");
    CHECK(ctl::formatTrigger(*k) == "key:ctrl+shift+f9");
    CHECK(ctl::parseTrigger("key:f13"));
    CHECK_FALSE(ctl::parseTrigger("key:shift+ctrl+a")); // fixed modifier order
    CHECK_FALSE(ctl::parseTrigger("key:ctrl+"));
    CHECK_FALSE(ctl::parseTrigger("key:ctrl+hyper"));
    CHECK_FALSE(ctl::parseTrigger("key:ctrl+ctrl+a"));

    const auto cc = ctl::parseTrigger("midi:cc/1/7");
    REQUIRE(cc);
    CHECK(cc->midi == ctl::MidiKind::Cc);
    CHECK(cc->number == 7);
    CHECK(ctl::formatTrigger(*cc) == "midi:cc/1/7");
    CHECK(ctl::formatTrigger(*ctl::parseTrigger("midi:pb/16")) == "midi:pb/16");
    CHECK(ctl::parseTrigger("midi:note/10/0"));
    CHECK_FALSE(ctl::parseTrigger("midi:note/0/1"));
    CHECK_FALSE(ctl::parseTrigger("midi:note/1/128"));
    CHECK_FALSE(ctl::parseTrigger("midi:pb/1/3"));
    CHECK_FALSE(ctl::parseTrigger("osc:/a"));
    CHECK(ctl::parseTrigger("")->type == ctl::Trigger::Type::None);

    CHECK(ctl::virtualKeyOf("a") == 0x41);
    CHECK(ctl::virtualKeyOf("f24") == 0x87);
    CHECK(ctl::virtualKeyOf("num_add") == 0x6B);
    CHECK(ctl::keyNameOf(0x70) == "f1");
    CHECK(ctl::keyNameOf(0x10).empty()); // shift is a modifier, not a key
    for (const auto& name : ctl::keyNames()) CHECK(ctl::keyNameOf(ctl::virtualKeyOf(name)) == name);
}

TEST_CASE("control: MIDI messages decode into triggers with press and release", "[control]") {
    auto on = ctl::decodeMidi(0x90, 60, 100);
    REQUIRE(on);
    CHECK(on->press);
    CHECK(ctl::formatTrigger(ctl::triggerOf(*on)) == "midi:note/1/60");
    auto off0 = ctl::decodeMidi(0x92, 60, 0); // note on with velocity 0 is a release
    REQUIRE(off0);
    CHECK(off0->release);
    CHECK(off0->channel == 3);
    CHECK(ctl::decodeMidi(0x80, 60, 64)->release);
    auto cc = ctl::decodeMidi(0xB0, 7, 127);
    CHECK(cc->press);
    CHECK(ctl::normalizedValue(*cc) == 1.0);
    CHECK(ctl::decodeMidi(0xB0, 7, 10)->release);
    auto pb = ctl::decodeMidi(0xEF, 0x7F, 0x7F);
    CHECK(pb->value == 16383);
    CHECK(ctl::formatTrigger(ctl::triggerOf(*pb)) == "midi:pb/16");
    CHECK(ctl::decodeMidi(0xC0, 5, 0)->press);
    CHECK_FALSE(ctl::decodeMidi(0xF8, 0, 0)); // clock
    CHECK_FALSE(ctl::decodeMidi(0xFE, 0, 0)); // active sensing
    CHECK_FALSE(ctl::decodeMidi(0xA0, 1, 1)); // aftertouch
}

TEST_CASE("control: binding checks give off, invalid and ok", "[control]") {
    auto t = defaultTree();
    CHECK(stateOf(t, bind("", "record_toggle")) == "off");
    CHECK(stateOf(t, bind("key:f9", "none")) == "off");
    auto disabled = bind("key:f9", "record_toggle");
    disabled.enabled = false;
    CHECK(stateOf(t, disabled) == "off");
    CHECK(stateOf(t, bind("key:f9", "record_toggle")) == "ok");
    CHECK(stateOf(t, bind("key:bogus", "record_toggle")) == "invalid");
    CHECK(stateOf(t, bind("key:f9", "param", "/track/1/volume_db")) == "invalid"); // param is MIDI only
    CHECK(stateOf(t, bind("midi:pb/1", "toggle", "/track/1/mute")) == "invalid");
    CHECK(stateOf(t, bind("midi:pb/1", "param", "/track/1/pan")) == "ok");

    CHECK(stateOf(t, bind("key:f1", "pad", "5")) == "ok");
    CHECK(stateOf(t, bind("key:f1", "pad", "8/16")) == "ok");
    CHECK(stateOf(t, bind("key:f1", "pad", "9/1")) == "invalid");
    CHECK(stateOf(t, bind("key:f1", "pad", "17")) == "invalid");
    CHECK(stateOf(t, bind("key:f1", "pad_stop", "")) == "ok");
    CHECK(stateOf(t, bind("key:f1", "pad_stop", "9")) == "invalid");
    CHECK(stateOf(t, bind("key:f1", "take", "1")) == "ok");
    CHECK(stateOf(t, bind("key:f1", "take", "2")) == "invalid"); // no mix 2 yet

    CHECK(stateOf(t, bind("key:m", "toggle", "/track/1/mute")) == "ok");
    CHECK(stateOf(t, bind("key:m", "toggle", "/track/2/mute")) == "invalid"); // no track 2
    CHECK(stateOf(t, bind("key:m", "toggle", "/track/1/pan")) == "invalid");
    CHECK(stateOf(t, bind("key:m", "toggle", "/capture/state")) == "invalid"); // read-only
    CHECK(stateOf(t, bind("key:m", "toggle", "/control/binding/1/enabled")) == "invalid");
    CHECK(stateOf(t, bind("key:m", "set", "/sampler/active_bank", "3")) == "ok");
    CHECK(stateOf(t, bind("key:m", "set", "/sampler/active_bank", "9")) == "invalid");
    CHECK(stateOf(t, bind("key:m", "set", "/video/mix/1/transition/type", "fade")) == "ok");
    CHECK(stateOf(t, bind("key:m", "step", "/track/1/volume_db", "-1")) == "ok");
    CHECK(stateOf(t, bind("key:m", "step", "/track/1/volume_db", "")) == "invalid");
    CHECK(stateOf(t, bind("key:m", "step", "/track/1/mute", "1")) == "invalid");
    CHECK(stateOf(t, bind("midi:cc/1/1", "param", "/video/mix/1/transition/type")) == "ok");
    CHECK(stateOf(t, bind("midi:cc/1/1", "param", "/track/1/name")) == "invalid"); // free text
}

TEST_CASE("control: MIDI values, steps and set values become parameter values", "[control]") {
    auto t = defaultTree();
    auto f = [&](const char* path, double n) { return std::get<double>(*ctl::valueFromMidi(t, path, n)); };
    CHECK(f("/track/1/volume_db", 0.0) == -144.0);
    CHECK(f("/track/1/volume_db", 1.0) == Approx(12.0));
    CHECK(f("/track/1/volume_db", 1.0 / 127) == Approx(-60.0 + 72.0 / 127));
    CHECK(f("/track/1/volume_db", 106.0 / 127) == Approx(0.0).margin(0.2)); // CC 106 ~ 0 dB
    CHECK(f("/track/1/pan", 0.5) == Approx(0.0));
    CHECK(f("/track/1/lowcut/freq_hz", 0.0) == Approx(20.0));
    CHECK(std::get<bool>(*ctl::valueFromMidi(t, "/track/1/mute", 0.7)) == true);
    CHECK(std::get<std::string>(*ctl::valueFromMidi(t, "/video/mix/1/transition/type", 1.0)) == "fade");
    CHECK(std::get<std::int64_t>(*ctl::valueFromMidi(t, "/sampler/active_bank", 1.0)) == 8);
    CHECK(std::get<std::int64_t>(*ctl::valueFromMidi(t, "/track/1/channels", 0.2)) == 1); // int choices

    // built-in effect parameters use the effect's range, logarithmic where the effect says so
    t.set("/track/1/fx/1/type", std::string("eq4"));
    t.set("/track/1/fx/1/param/low_freq_hz", 100.0);
    const auto lo = ctl::numberRange(t, "/track/1/fx/1/param/low_freq_hz");
    REQUIRE(lo);
    CHECK(lo->log);
    CHECK(f("/track/1/fx/1/param/low_freq_hz", 0.0) == Approx(lo->min));
    CHECK(f("/track/1/fx/1/param/low_freq_hz", 0.5) == Approx(std::sqrt(lo->min * lo->max)));
    t.set("/track/1/fx/2/type", std::string("vst3:abc"));
    t.set("/track/1/fx/2/param/p7", 0.0);
    CHECK(f("/track/1/fx/2/param/p7", 0.25) == Approx(0.25));

    // steps clamp, and stepping a silent fader up starts at the bottom of the fader
    t.set("/track/1/volume_db", 11.5);
    CHECK(std::get<double>(*ctl::stepValue(t, "/track/1/volume_db", 1.0)) == 12.0);
    t.set("/track/1/volume_db", -144.0);
    CHECK(std::get<double>(*ctl::stepValue(t, "/track/1/volume_db", 1.0)) == Approx(-59.0));
    t.set("/track/1/volume_db", -59.5);
    CHECK(std::get<double>(*ctl::stepValue(t, "/track/1/volume_db", -1.0)) == -144.0);
    CHECK(std::get<std::int64_t>(*ctl::stepValue(t, "/sampler/active_bank", 1.0)) == 2);

    CHECK(ctl::parseValue(t, "/track/1/mute", "on") == proto::Value{true});
    CHECK(ctl::parseValue(t, "/sampler/active_bank", "4") == proto::Value{std::int64_t{4}});
    CHECK_FALSE(ctl::parseValue(t, "/sampler/active_bank", "4.5"));
    CHECK(ctl::parseValue(t, "/track/1/pan", "1") == proto::Value{1.0});
    CHECK_FALSE(ctl::parseValue(t, "/track/1/pan", "2"));

    CHECK(ctl::faderToDb(ctl::dbToFader(-6.0)) == Approx(-6.0));
}

namespace {

struct Rig {
    asio::io_context io;
    proto::ParamStore store;
    Controller* ctlPtr = nullptr;
    ControlBindings* bindPtr = nullptr;
    std::unique_ptr<control::ControlServer> server;
    FakeBackend backend;
    Engine engine{nullptr};
    std::unique_ptr<DeviceManager> devices;
    std::unique_ptr<Controller> ctl;
    FakeHotkeys keys;
    FakeMidi midi;
    std::unique_ptr<ControlBindings> bindings;
    std::unique_ptr<control::ControlClient> client;
    std::thread net;
    asio::executor_work_guard<asio::io_context::executor_type> work = asio::make_work_guard(io);

    explicit Rig(std::string storePath = {}) {
        control::ControlServer::Options opt;
        opt.port = 0;
        opt.onDelta = [this](const proto::Delta& d) {
            if (ctlPtr) ctlPtr->onDelta(d);
            if (bindPtr) bindPtr->onDelta(d);
        };
        server = std::make_unique<control::ControlServer>(io, store, opt);
        devices = std::make_unique<DeviceManager>(backend, engine, DeviceManager::Options{});
        Controller::Options co;
        co.verbose = false;
        ctl = std::make_unique<Controller>(io, *server, *devices, engine, nullptr, co);
        midi.setDevices({"Pads", "Faders"});
        ControlBindings::Options bo;
        bo.storePath = std::move(storePath);
        bo.verbose = false;
        bo.midiRescan = 100ms;
        bo.learnTimeout = 500ms;
        bindings = std::make_unique<ControlBindings>(io, *server, keys, midi, bo);
        server->start();
        ctl->start(devices->enumerateNow());
        ctlPtr = ctl.get();
        bindings->start();
        bindPtr = bindings.get();
        net = std::thread([this] { io.run(); });
        control::ControlClient::Options o;
        o.port = server->port();
        o.clientName = "test";
        client = std::make_unique<control::ControlClient>(o);
    }

    ~Rig() {
        client.reset();
        asio::post(io, [this] {
            bindPtr = nullptr;
            bindings->stop();
            ctl->stop();
            server->stop();
        });
        work.reset();
        std::this_thread::sleep_for(50ms);
        io.stop();
        net.join();
    }

    template <typename Pred>
    bool waitFor(Pred pred) {
        for (int i = 0; i < 100; ++i) {
            if (pred(client->tree())) return true;
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }
    std::int64_t add(proto::Args args) {
        auto r = client->call("/control/binding/add", std::move(args)).get();
        REQUIRE(r.code == proto::ErrorCode::Ok);
        return std::get<std::int64_t>(r.value);
    }
    std::string state(std::int64_t n) {
        return client->tree().getAs<std::string>("/control/binding/" + std::to_string(n) + "/state").value_or("");
    }
};

proto::Args args(std::initializer_list<std::pair<const char*, const char*>> kv) {
    proto::Args a;
    for (const auto& [k, v] : kv) a[k] = std::string(v);
    return a;
}

} // namespace

TEST_CASE("control bindings: hotkeys run actions, conflicts show, learn fills the trigger", "[control][integration]") {
    Rig rig;
    REQUIRE(rig.client->connect(3s));

    const auto mute = rig.add(args({{"trigger", "key:ctrl+m"}, {"action", "toggle"}, {"target", "/track/1/mute"}}));
    CHECK(mute == 1);
    REQUIRE(rig.waitFor([&](const proto::ParamTree&) { return rig.state(mute) == "ok"; }));
    CHECK(rig.keys.registeredCount() == 1);

    REQUIRE(rig.keys.press(ctl::ModCtrl, 'M'));
    CHECK(rig.waitFor([](const proto::ParamTree& t) { return t.getAs<bool>("/track/1/mute") == true; }));
    REQUIRE(rig.keys.press(ctl::ModCtrl, 'M', true)); // release does nothing for toggle
    REQUIRE(rig.keys.press(ctl::ModCtrl, 'M'));
    CHECK(rig.waitFor([](const proto::ParamTree& t) { return t.getAs<bool>("/track/1/mute") == false; }));

    // the same combination for a second binding: one registration, both run
    const auto bank = rig.add(args({{"trigger", "key:ctrl+m"}, {"action", "set"}, {"target", "/sampler/active_bank"}, {"value", "3"}}));
    REQUIRE(rig.waitFor([&](const proto::ParamTree&) { return rig.state(bank) == "ok"; }));
    CHECK(rig.keys.registeredCount() == 1);
    REQUIRE(rig.keys.press(ctl::ModCtrl, 'M'));
    CHECK(rig.waitFor([](const proto::ParamTree& t) {
        return t.getAs<std::int64_t>("/sampler/active_bank") == 3 && t.getAs<bool>("/track/1/mute") == true;
    }));

    // step: auto-repeat accepted; the volume clamps
    const auto down = rig.add(args({{"trigger", "key:f13"}, {"action", "step"}, {"target", "/master/volume_db"}, {"value", "-1.5"}}));
    REQUIRE(rig.waitFor([&](const proto::ParamTree&) { return rig.state(down) == "ok"; }));
    REQUIRE(rig.keys.press(0, 0x7C));
    REQUIRE(rig.keys.press(0, 0x7C, false, true));
    CHECK(rig.waitFor([](const proto::ParamTree& t) { return t.getAs<double>("/master/volume_db") == Approx(-3.0); }));

    // taken by another application
    rig.keys.setTaken({{ctl::ModAlt, 'R'}});
    const auto rec = rig.add(args({{"trigger", "key:alt+r"}, {"action", "record_toggle"}}));
    REQUIRE(rig.waitFor([&](const proto::ParamTree&) { return rig.state(rec) == "conflict"; }));
    rig.keys.setTaken({}); // retried with the MIDI rescan
    CHECK(rig.waitFor([&](const proto::ParamTree&) { return rig.state(rec) == "ok"; }));

    // invalid target, then fixed by a Set
    const auto bad = rig.add(args({{"trigger", "key:f14"}, {"action", "toggle"}, {"target", "/track/9/mute"}}));
    REQUIRE(rig.waitFor([&](const proto::ParamTree&) { return rig.state(bad) == "invalid"; }));
    CHECK_FALSE(rig.client->tree().getAs<std::string>("/control/binding/" + std::to_string(bad) + "/error").value_or("").empty());
    REQUIRE(rig.client->set({{"/control/binding/" + std::to_string(bad) + "/target", std::string("/track/1/solo")}}).get().code ==
            proto::ErrorCode::Ok);
    CHECK(rig.waitFor([&](const proto::ParamTree&) { return rig.state(bad) == "ok"; }));

    // disabling unregisters
    const auto before = rig.keys.registeredCount();
    REQUIRE(rig.client->set({{"/control/binding/" + std::to_string(bad) + "/enabled", false}}).get().code == proto::ErrorCode::Ok);
    CHECK(rig.waitFor([&](const proto::ParamTree&) { return rig.state(bad) == "off" && rig.keys.registeredCount() == before - 1; }));

    // learn a key combination
    const auto learnt = rig.add(args({{"action", "record_toggle"}}));
    CHECK(rig.state(learnt) == "off");
    REQUIRE(rig.client->call("/control/learn", {{"binding", learnt}, {"source", std::string("key")}}).get().code ==
            proto::ErrorCode::Ok);
    CHECK(rig.client->tree().getAs<std::int64_t>("/control/learn/binding") == learnt);
    CHECK(rig.waitFor([&](const proto::ParamTree&) { return rig.keys.learning.load(); }));
    rig.keys.learn("key:ctrl+shift+f9");
    CHECK(rig.waitFor([&](const proto::ParamTree& t) {
        return t.getAs<std::string>("/control/binding/" + std::to_string(learnt) + "/trigger") == "key:ctrl+shift+f9" &&
               t.getAs<std::int64_t>("/control/learn/binding") == 0 && rig.state(learnt) == "ok";
    }));
    CHECK_FALSE(rig.keys.learning.load());

    // learning ends by itself after the timeout
    REQUIRE(rig.client->call("/control/learn", {{"binding", learnt}}).get().code == proto::ErrorCode::Ok);
    CHECK(rig.waitFor([](const proto::ParamTree& t) { return t.getAs<std::int64_t>("/control/learn/binding") == 0; }));

    // remove
    REQUIRE(rig.client->call("/control/binding/remove", {{"binding", mute}}).get().code == proto::ErrorCode::Ok);
    CHECK(rig.waitFor([](const proto::ParamTree& t) { return !t.contains("/control/binding/1/action"); }));
    CHECK(rig.client->call("/control/binding/remove", {{"binding", mute}}).get().code == proto::ErrorCode::NotFound);
    CHECK(rig.client->call("/control/binding/add", {{"action", std::string("explode")}}).get().code == proto::ErrorCode::OutOfRange);
    // the freed number is reused
    CHECK(rig.add(args({})) == 1);
}

TEST_CASE("control bindings: MIDI devices, buttons on edges, faders, learn", "[control][integration]") {
    Rig rig;
    REQUIRE(rig.client->connect(3s));
    REQUIRE(rig.waitFor([](const proto::ParamTree& t) {
        return t.getAs<std::string>("/control/midi/device/1/name") == "Pads" &&
               t.getAs<std::string>("/control/midi/device/2/state") == "ok";
    }));
    CHECK(rig.midi.isOpen("Pads"));

    // a pad button toggles mute once per press, even when it sends 127 twice
    const auto b = rig.add(args({{"trigger", "midi:cc/1/20"}, {"action", "toggle"}, {"target", "/track/1/mute"}}));
    REQUIRE(rig.waitFor([&](const proto::ParamTree&) { return rig.state(b) == "ok"; }));
    rig.midi.send("Pads", 0xB0, 20, 127);
    rig.midi.send("Pads", 0xB0, 20, 127);
    CHECK(rig.waitFor([](const proto::ParamTree& t) { return t.getAs<bool>("/track/1/mute") == true; }));
    std::this_thread::sleep_for(100ms);
    CHECK(rig.client->tree().getAs<bool>("/track/1/mute") == true);
    CHECK(rig.waitFor([](const proto::ParamTree& t) {
        return t.getAs<std::string>("/control/midi/last/trigger") == "midi:cc/1/20" &&
               t.getAs<std::string>("/control/midi/last/device") == "Pads" && t.getAs<std::int64_t>("/control/midi/last/value") == 127;
    }));

    // a fader on another device only when the binding names that device
    const auto fader = rig.add(args({{"trigger", "midi:cc/1/7"}, {"device", "Faders"}, {"action", "param"}, {"target", "/track/1/volume_db"}}));
    REQUIRE(rig.waitFor([&](const proto::ParamTree&) { return rig.state(fader) == "ok"; }));
    rig.midi.send("Pads", 0xB0, 7, 0);
    rig.midi.send("Faders", 0xB0, 7, 127);
    CHECK(rig.waitFor([](const proto::ParamTree& t) { return t.getAs<double>("/track/1/volume_db") == Approx(12.0); }));
    for (int v = 126; v >= 0; --v) rig.midi.send("Faders", 0xB0, 7, static_cast<std::uint8_t>(v)); // a fast move: the last value wins
    CHECK(rig.waitFor([](const proto::ParamTree& t) { return t.getAs<double>("/track/1/volume_db") == -144.0; }));

    // learn from MIDI: device and trigger are filled
    const auto learnt = rig.add(args({{"action", "set"}, {"target", "/sampler/active_bank"}, {"value", "5"}}));
    REQUIRE(rig.client->call("/control/learn", {{"binding", learnt}, {"source", std::string("midi")}}).get().code ==
            proto::ErrorCode::Ok);
    rig.midi.send("Pads", 0x99, 36, 90);
    CHECK(rig.waitFor([&](const proto::ParamTree& t) {
        const std::string base = "/control/binding/" + std::to_string(learnt);
        return t.getAs<std::string>(base + "/trigger") == "midi:note/10/36" && t.getAs<std::string>(base + "/device") == "Pads" &&
               rig.state(learnt) == "ok";
    }));
    rig.midi.send("Pads", 0x89, 36, 0); // the release of the learning press does nothing
    rig.midi.send("Pads", 0x99, 36, 90);
    CHECK(rig.waitFor([](const proto::ParamTree& t) { return t.getAs<std::int64_t>("/sampler/active_bank") == 5; }));

    // a device turned off is closed and remembered; one held by another application is busy
    REQUIRE(rig.client->set({{"/control/midi/device/2/enabled", false}}).get().code == proto::ErrorCode::Ok);
    CHECK(rig.waitFor([&](const proto::ParamTree& t) {
        return t.getAs<std::string>("/control/midi/device/2/state") == "off" && !rig.midi.isOpen("Faders");
    }));
    rig.midi.setBusy({"Keys"});
    rig.midi.setDevices({"Pads", "Faders", "Keys"});
    CHECK(rig.waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/control/midi/device/3/state") == "busy"; }));
    rig.midi.setBusy({});
    CHECK(rig.waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/control/midi/device/3/state") == "ok"; }));
    rig.midi.setDevices({"Faders"});
    CHECK(rig.waitFor([&](const proto::ParamTree& t) {
        return t.getAs<std::string>("/control/midi/device/1/name") == "Faders" && !t.contains("/control/midi/device/2/name") &&
               t.getAs<bool>("/control/midi/device/1/enabled") == false && !rig.midi.isOpen("Pads");
    }));
}

TEST_CASE("control bindings: saved and loaded again", "[control][integration]") {
    const auto dir = std::filesystem::temp_directory_path() / "dc-control-test";
    std::filesystem::remove_all(dir);
    const auto file = (dir / "mixd-control.txt").string();
    {
        Rig rig(file);
        REQUIRE(rig.client->connect(3s));
        rig.add(args({{"trigger", "key:ctrl+m"}, {"action", "toggle"}, {"target", "/track/1/mute"}}));
        const auto two = rig.add(args({{"trigger", "midi:note/1/40"}, {"device", "Pads"}, {"action", "pad"}, {"target", "2/3"}}));
        REQUIRE(rig.client->set({{"/control/binding/" + std::to_string(two) + "/enabled", false},
                                 {"/control/midi/device/1/enabled", false}})
                    .get()
                    .code == proto::ErrorCode::Ok);
        CHECK(rig.waitFor([&](const proto::ParamTree&) { return std::filesystem::exists(file); }));
        std::this_thread::sleep_for(1200ms); // the save after the last change
    }
    Rig again(file);
    REQUIRE(again.client->connect(3s));
    const auto& t = again.client->tree();
    CHECK(t.getAs<std::string>("/control/binding/1/trigger") == "key:ctrl+m");
    CHECK(again.state(1) == "ok");
    CHECK(t.getAs<std::string>("/control/binding/2/target") == "2/3");
    CHECK(t.getAs<std::string>("/control/binding/2/device") == "Pads");
    CHECK(t.getAs<bool>("/control/binding/2/enabled") == false);
    CHECK(t.getAs<bool>("/control/midi/device/1/enabled") == false);
    CHECK_FALSE(again.midi.isOpen("Pads"));
    std::filesystem::remove_all(dir);
}
