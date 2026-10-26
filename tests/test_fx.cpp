// Effects in the engine (lane A1): the built-in DSP, the bypass crossfade, and the effect racks
// behind the control server with a fake plugin host standing in for VST3.

#include "controller.hpp"
#include "device_manager.hpp"
#include "engine.hpp"
#include "fake_backend.hpp"
#include "fx/builtin.hpp"
#include "fx/plugin_host.hpp"
#include "profile_manager.hpp"

#include "dc/control/client.hpp"
#include "dc/protocol/fx.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace dc;
using namespace dc::mixd;
using namespace std::chrono_literals;
using Catch::Approx;

namespace {

double toDb(double v) { return 20 * std::log10(std::max(v, 1e-12)); }

// Runs a sine of amplitude amp through fx for seconds; returns the output peak of the last 100 ms.
double sinePeak(Effect& fx, double freq, double amp, int channels = 2, double seconds = 1.0) {
    const int total = static_cast<int>(seconds * kSampleRate);
    std::vector<float> l(256), r(256);
    double phase = 0, peak = 0;
    for (int done = 0; done < total; done += 256) {
        for (int i = 0; i < 256; ++i) {
            l[i] = r[i] = static_cast<float>(amp * std::sin(phase));
            phase += 2 * kPi * freq / kSampleRate;
        }
        float* x[2] = {l.data(), r.data()};
        fx.process(x, channels, 256);
        if (done >= total - 4800)
            for (int i = 0; i < 256; ++i) peak = std::max(peak, static_cast<double>(std::fabs(l[i])));
    }
    return peak;
}

// Feeds an impulse on the left channel and returns n output samples per channel.
std::array<std::vector<float>, 2> impulse(Effect& fx, int n, int channels = 2) {
    std::array<std::vector<float>, 2> out{std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f)};
    out[0][0] = 1.0f;
    for (int off = 0; off < n; off += 256) {
        const int len = std::min(256, n - off);
        float* x[2] = {out[0].data() + off, out[1].data() + off};
        fx.process(x, channels, len);
    }
    return out;
}

std::shared_ptr<BuiltinEffect> make(const char* type, std::initializer_list<std::pair<const char*, double>> params) {
    auto fx = makeBuiltinEffect(type);
    REQUIRE(fx);
    for (auto& [k, v] : params) fx->setParam(k, v);
    fx->reset();
    return fx;
}

} // namespace

TEST_CASE("eq4 boosts and cuts its bands and leaves the rest alone", "[fx]") {
    auto eq = make("eq4", {{"mid1_freq_hz", 1000}, {"mid1_gain_db", 12}, {"mid1_q", 2}});
    CHECK(toDb(sinePeak(*eq, 1000, 0.1)) - toDb(0.1) == Approx(12).margin(0.3));
    CHECK(toDb(sinePeak(*eq, 100, 0.1)) - toDb(0.1) == Approx(0).margin(0.5));
    auto shelf = make("eq4", {{"low_freq_hz", 200}, {"low_gain_db", -12}, {"high_gain_db", 6}, {"high_freq_hz", 2000}});
    CHECK(toDb(sinePeak(*shelf, 40, 0.1)) - toDb(0.1) == Approx(-12).margin(0.6));
    CHECK(toDb(sinePeak(*shelf, 12000, 0.1)) - toDb(0.1) == Approx(6).margin(0.6));
    CHECK(toDb(sinePeak(*shelf, 700, 0.1)) - toDb(0.1) == Approx(0).margin(1.5));
    auto out = make("eq4", {{"output_db", -6}});
    CHECK(toDb(sinePeak(*out, 1000, 0.1, 1)) - toDb(0.1) == Approx(-6).margin(0.1));
}

TEST_CASE("compressor reduces above the threshold by the ratio", "[fx]") {
    auto comp = make("comp", {{"threshold_db", -20}, {"ratio", 4}, {"knee_db", 0}, {"attack_ms", 1}, {"release_ms", 200}});
    // 0 dBFS in: 20 dB over, 4:1 -> 15 dB of reduction
    CHECK(toDb(sinePeak(*comp, 1000, 1.0)) == Approx(-15).margin(1.0));
    CHECK(comp->gainReductionDb() == Approx(-15).margin(1.0));
    // well below the threshold: untouched
    auto comp2 = make("comp", {{"threshold_db", -20}, {"ratio", 4}, {"makeup_db", 3}});
    CHECK(toDb(sinePeak(*comp2, 1000, 0.01)) - toDb(0.01) == Approx(3).margin(0.1));
}

TEST_CASE("gate closes below the threshold and opens above it", "[fx]") {
    auto gate = make("gate", {{"threshold_db", -40}, {"floor_db", -90}});
    CHECK(sinePeak(*gate, 500, 0.001) < 1e-5); // -60 dBFS: closed (floor -90 = silence)
    CHECK(sinePeak(*gate, 500, 0.1) == Approx(0.1).margin(0.002));
    auto soft = make("gate", {{"threshold_db", -40}, {"floor_db", -20}});
    CHECK(toDb(sinePeak(*soft, 500, 0.001)) - toDb(0.001) == Approx(-20).margin(0.5));
}

TEST_CASE("reverb leaves a decaying tail; mix 0 is dry", "[fx]") {
    auto rev = make("reverb", {{"size", 0.7}, {"mix", 0.5}, {"predelay_ms", 20}});
    const auto out = impulse(*rev, 96000);
    auto energy = [&](int from, int to) {
        double e = 0;
        for (int i = from; i < to; ++i) e += out[0][i] * out[0][i] + out[1][i] * out[1][i];
        return e;
    };
    // nothing wet before the pre-delay (only the dry impulse at 0)
    CHECK(energy(1, 900) < 1e-9);
    CHECK(energy(4800, 9600) > 1e-5);
    CHECK(energy(48000, 52800) < energy(4800, 9600) * 0.1);
    auto dry = make("reverb", {{"mix", 0}});
    const auto d = impulse(*dry, 4800);
    CHECK(d[0][0] == 1.0f);
    double rest = 0;
    for (int i = 1; i < 4800; ++i) rest += std::fabs(d[0][i]) + std::fabs(d[1][i]);
    CHECK(rest == 0);
}

TEST_CASE("delay repeats after its time; ping-pong alternates the sides", "[fx]") {
    auto dl = make("delay", {{"time_ms", 100}, {"feedback", 0.5}, {"mix", 1}, {"highcut_hz", 20000}});
    auto out = impulse(*dl, 20000);
    CHECK(std::fabs(out[0][4800]) == Approx(1.0).margin(0.02));
    CHECK(std::fabs(out[0][9600]) == Approx(0.5).margin(0.05));
    CHECK(std::fabs(out[1][4800]) == Approx(0.0).margin(1e-6)); // stereo: the right side got no input

    auto pp = make("delay", {{"time_ms", 100}, {"feedback", 0.5}, {"mix", 1}, {"highcut_hz", 20000}, {"ping_pong", 1}});
    out = impulse(*pp, 20000);
    CHECK(std::fabs(out[0][4800]) == Approx(0.5).margin(0.02)); // (L + R) / 2 into the left line
    CHECK(std::fabs(out[1][4800]) < 1e-4);
    CHECK(std::fabs(out[1][9600]) == Approx(0.25).margin(0.03));
    CHECK(std::fabs(out[0][9600]) < 1e-3);
}

TEST_CASE("limiter never exceeds the ceiling and delays by its look-ahead", "[fx]") {
    auto lim = make("limiter", {{"ceiling_db", -1}, {"input_db", 6}});
    const double peak = sinePeak(*lim, 1000, 1.0);
    CHECK(peak <= std::pow(10.0, -1.0 / 20) + 1e-6);
    CHECK(toDb(peak) == Approx(-1).margin(0.3));
    auto lim2 = make("limiter", {});
    const auto out = impulse(*lim2, 200);
    CHECK(out[0][48] == Approx(std::pow(10.0, -1.0 / 20)).margin(0.01)); // 1.0 limited to the ceiling, 48 late
    for (int i = 0; i < 48; ++i) CHECK(out[0][i] == 0.0f);
    CHECK(lim2->latencySamples() == 48);
}

TEST_CASE("every built-in effect stays finite while its parameters move", "[fx]") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (const auto& spec : proto::builtinFx()) {
        for (int channels : {1, 2}) {
            INFO(spec.type << " channels " << channels);
            auto fx = makeBuiltinEffect(spec.type);
            std::vector<float> l(256), r(256);
            float peak = 0;
            bool finite = true;
            for (int block = 0; block < 400; ++block) {
                if (block % 10 == 0)
                    for (const auto& p : spec.params) {
                        std::uniform_real_distribution<double> v(p.min, p.max);
                        fx->setParam(p.key, v(rng));
                    }
                for (int i = 0; i < 256; ++i) l[i] = r[i] = noise(rng) * 0.5f;
                float* x[2] = {l.data(), r.data()};
                fx->process(x, channels, block % 3 == 0 ? 37 : 256);
                for (int i = 0; i < 256; ++i) {
                    finite = finite && std::isfinite(l[i]) && std::isfinite(r[i]);
                    peak = std::max({peak, std::fabs(l[i]), std::fabs(r[i])});
                }
            }
            CHECK(finite);
            CHECK(peak < 100.0f);
        }
    }
}

TEST_CASE("bypass crossfades and skips the effect", "[fx]") {
    auto eq = make("eq4", {{"output_db", -6}});
    FxSlot slot(eq);
    auto run = [&](float in) {
        std::vector<float> l(480, in), r(480, in);
        float* x[2] = {l.data(), r.data()};
        slot.process(x, 2, 480);
        return std::array<float, 3>{l.front(), l[240], l.back()};
    };
    for (int i = 0; i < 20; ++i) run(1.0f);
    CHECK(run(1.0f)[2] == Approx(0.501).margin(0.01));
    slot.setBypass(true);
    const auto fade = run(1.0f); // the 10 ms crossfade, monotonic towards dry
    CHECK(fade[0] < fade[1]);
    CHECK(fade[1] < fade[2]);
    CHECK(run(1.0f)[2] == 1.0f);
    slot.setBypass(false);
    run(1.0f);
    CHECK(run(1.0f)[2] == Approx(0.501).margin(0.01));
}

namespace {

// A plugin host with one fake VST3 effect: a gain controlled by parameter p7 (gain = 2 * value),
// 300 parameters in all.
class FakePlugin final : public PluginInstance {
public:
    std::string name() const override { return "Fake Gain"; }
    void setListener(Listener l) override {
        std::lock_guard lock(mutex);
        listener = std::move(l);
    }
    int paramCount() const override { return 300; }
    std::vector<PluginParamInfo> params(int offset, int count) const override {
        std::vector<PluginParamInfo> out;
        for (int i = offset; i < offset + count && i < 300; ++i) out.push_back(*param(proto::vst3ParamKey(static_cast<std::uint32_t>(i))));
        return out;
    }
    std::optional<PluginParamInfo> param(const std::string& key) const override {
        auto id = proto::vst3ParamId(key);
        if (!id || *id >= 300) return std::nullopt;
        PluginParamInfo p;
        p.key = key;
        p.name = "Param " + std::to_string(*id);
        p.value = values[*id].load();
        p.defaultValue = *id == 7 ? 0.5 : 0.0;
        p.text = std::to_string(p.value);
        return p;
    }
    bool hasEditor() const override { return true; }
    void setEditorOpen(bool open) override { editor = open; }
    void setParam(std::string_view key, double value) override {
        if (auto id = proto::vst3ParamId(key); id && *id < 300) values[*id] = value;
    }
    void process(float* const* x, int channels, int frames) override {
        const float g = static_cast<float>(values[7].load() * 2);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < frames; ++i) x[c][i] *= g;
    }
    void reset() override {}
    // the whole state: every value, as text
    std::string state() const override {
        std::string out = "fake:";
        for (const auto& v : values) out += std::to_string(v.load()) + ",";
        return out;
    }
    bool setState(const std::string& data) override {
        if (data.rfind("fake:", 0) != 0) return false;
        std::size_t pos = 5;
        for (auto& v : values) {
            const auto comma = data.find(',', pos);
            if (comma == std::string::npos) return false;
            v = std::stod(data.substr(pos, comma - pos));
            pos = comma + 1;
        }
        ++restored;
        return true;
    }
    std::atomic<int> restored{0};
    // the plugin's own window moves a knob
    void edit(int id, double v) {
        values[static_cast<std::size_t>(id)] = v;
        std::lock_guard lock(mutex);
        if (listener.onParam) listener.onParam(proto::vst3ParamKey(static_cast<std::uint32_t>(id)), v);
    }
    void closeWindow() {
        editor = false;
        std::lock_guard lock(mutex);
        if (listener.onEditorClosed) listener.onEditorClosed();
    }

    FakePlugin() {
        for (auto& v : values) v = 0.0;
        values[7] = 0.5;
    }
    std::array<std::atomic<double>, 300> values;
    std::atomic<bool> editor{false};
    std::mutex mutex;
    Listener listener;
};

class FakeHost final : public PluginHost {
public:
    std::vector<PluginInfo> plugins() const override {
        PluginInfo ok{"vst3:00000007", "Fake Gain", "Test", "Fx", "C:/fake.vst3", false, "ok", ""};
        PluginInfo bad{"vst3:000000ff", "Crasher", "Test", "Fx", "C:/crash.vst3", false, "crashed", "access violation"};
        return {ok, bad};
    }
    bool scan(bool, const std::vector<std::string>& dirs, std::function<void(ScanProgress)> progress,
              std::function<void()> done) override {
        scannedDirs = dirs;
        progress({0.5, "C:/fake.vst3"});
        done();
        return true;
    }
    void create(const std::string& id, std::function<void(std::shared_ptr<PluginInstance>, std::string)> done) override {
        if (id != "vst3:00000007") return done(nullptr, "cannot load");
        last = std::make_shared<FakePlugin>();
        done(last, "");
    }
    std::shared_ptr<FakePlugin> last;
    std::vector<std::string> scannedDirs;
};

} // namespace

TEST_CASE("effect racks behind the control server", "[fx][integration]") {
    asio::io_context io;
    proto::ParamStore store;
    Controller* ctlPtr = nullptr;
    control::ControlServer::Options opt;
    opt.port = 0;
    opt.onDelta = [&](const proto::Delta& d) {
        if (ctlPtr) ctlPtr->onDelta(d);
    };
    control::ControlServer server(io, store, opt);
    FakeBackend backend;
    Engine engine(nullptr);
    DeviceManager devices(backend, engine, {});
    FakeHost host;
    Controller::Options co;
    co.verbose = false;
    co.plugins = &host;
    Controller ctl(io, server, devices, engine, nullptr, co);
    server.start();
    ctl.start(devices.enumerateNow());
    ctlPtr = &ctl;
    devices.start([&](const DeviceManager::State& s) { ctl.onDeviceState(s); });
    auto work = asio::make_work_guard(io);
    std::thread net([&] { io.run(); });

    control::ControlClient::Options o;
    o.port = server.port();
    o.clientName = "test";
    control::ControlClient client(o);
    REQUIRE(client.connect(3s));
    auto waitFor = [&](auto pred) {
        for (int i = 0; i < 100; ++i) {
            if (pred(client.tree())) return true;
            std::this_thread::sleep_for(30ms);
        }
        return false;
    };
    using proto::ErrorCode;
    auto tree = [&] { return client.tree(); };

    // the plugin list from the host's cache
    CHECK(tree().getAs<std::string>("/plugins/1/name") == "Crasher"); // sorted by name
    CHECK(tree().getAs<std::string>("/plugins/2/id") == "vst3:00000007");
    CHECK(tree().getAs<std::string>("/plugins/1/status") == "crashed");

    // the line input on track 1 reaches the speaker at 0.25
    REQUIRE(client.set({{"/track/1/input/kind", std::string("device")}, {"/track/1/input/source", std::string("fake-line")}})
                .get()
                .code == ErrorCode::Ok);
    REQUIRE(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/track/1/input/state") == "ok"; }));
    std::this_thread::sleep_for(800ms);
    backend.takeOutputPeak("fake-spk");
    std::this_thread::sleep_for(200ms);
    CHECK(backend.takeOutputPeak("fake-spk") == Approx(0.25).margin(0.03));

    // built-in: eq4 at the end, comp in front of it
    auto r = client.call("/track/1/fx/add", {{"type", std::string("eq4")}}).get();
    REQUIRE(r.code == ErrorCode::Ok);
    CHECK(std::get<std::int64_t>(r.value) == 1);
    CHECK(tree().getAs<std::string>("/track/1/fx/1/name") == "4バンドEQ");
    CHECK(tree().getAs<double>("/track/1/fx/1/param/mid1_freq_hz") == 400.0);
    CHECK(tree().getAs<std::string>("/track/1/fx/1/status") == "ok");
    REQUIRE(client.set({{"/track/1/fx/1/param/output_db", -6.0}}).get().code == ErrorCode::Ok);
    std::this_thread::sleep_for(300ms);
    backend.takeOutputPeak("fake-spk");
    std::this_thread::sleep_for(200ms);
    CHECK(backend.takeOutputPeak("fake-spk") == Approx(0.125).margin(0.02));
    CHECK(client.set({{"/track/1/fx/1/param/output_db", 30.0}}).get().code == ErrorCode::OutOfRange);
    CHECK(client.set({{"/track/1/fx/1/editor", true}}).get().code == ErrorCode::Conflict);

    r = client.call("/track/1/fx/add", {{"type", std::string("comp")}, {"index", std::int64_t{1}}}).get();
    REQUIRE(r.code == ErrorCode::Ok);
    CHECK(tree().getAs<std::string>("/track/1/fx/1/type") == "comp");
    CHECK(tree().getAs<std::string>("/track/1/fx/2/type") == "eq4");
    REQUIRE(client.set({{"/track/1/fx/1/param/threshold_db", 0.0}}).get().code == ErrorCode::Ok); // comp idle
    CHECK(tree().getAs<double>("/track/1/fx/2/param/output_db") == -6.0); // moved with its slot
    CHECK(client.call("/track/1/fx/add", {{"type", std::string("chorus")}}).get().code == ErrorCode::NotFound);
    CHECK(client.call("/track/1/fx/add", {{"type", std::string("eq4")}, {"index", std::int64_t{9}}}).get().code ==
          ErrorCode::OutOfRange);
    CHECK(client.call("/track/9/fx/add", {{"type", std::string("eq4")}}).get().code == ErrorCode::NotFound);

    // the parameter list as JSON
    r = client.call("/track/1/fx/2/params", {{"keys", std::string("output_db,mid1_freq_hz")}}).get();
    REQUIRE(r.code == ErrorCode::Ok);
    const auto json = std::get<std::string>(r.value);
    CHECK(json.find("\"key\":\"output_db\"") != std::string::npos);
    CHECK(json.find("\"text\":\"-6.0 dB\"") != std::string::npos);
    CHECK(json.find("\"log\":true") != std::string::npos);
    r = client.call("/track/1/fx/1/params", {{"offset", std::int64_t{1}}, {"count", std::int64_t{2}}}).get();
    CHECK(std::get<std::string>(r.value).find("\"key\":\"ratio\"") != std::string::npos);
    CHECK(std::get<std::string>(r.value).find("\"key\":\"attack_ms\"") != std::string::npos);
    CHECK(std::get<std::string>(r.value).find("threshold_db") == std::string::npos);

    // move and remove renumber the slots; bypass keeps the level
    REQUIRE(client.call("/track/1/fx/move", {{"from", std::int64_t{2}}, {"to", std::int64_t{1}}}).get().code == ErrorCode::Ok);
    CHECK(tree().getAs<std::string>("/track/1/fx/1/type") == "eq4");
    REQUIRE(client.set({{"/track/1/fx/1/bypass", true}}).get().code == ErrorCode::Ok);
    std::this_thread::sleep_for(300ms);
    backend.takeOutputPeak("fake-spk");
    std::this_thread::sleep_for(200ms);
    CHECK(backend.takeOutputPeak("fake-spk") == Approx(0.25).margin(0.03));
    REQUIRE(client.call("/track/1/fx/remove", {{"index", std::int64_t{1}}}).get().code == ErrorCode::Ok);
    CHECK(tree().getAs<std::string>("/track/1/fx/1/type") == "comp");
    CHECK_FALSE(tree().contains("/track/1/fx/2/type"));
    REQUIRE(client.call("/track/1/fx/remove", {{"index", std::int64_t{1}}}).get().code == ErrorCode::Ok);
    CHECK(tree().pathsUnder("/track/1/fx").empty());

    // VST3 through the fake host: loading, parameters in the tree on demand, edits in the plugin window
    CHECK(client.call("/master/fx/add", {{"type", std::string("vst3:000000ff")}}).get().code == ErrorCode::Conflict);
    CHECK(client.call("/master/fx/add", {{"type", std::string("vst3:12345678")}}).get().code == ErrorCode::NotFound);
    r = client.call("/master/fx/add", {{"type", std::string("vst3:00000007")}}).get();
    REQUIRE(r.code == ErrorCode::Ok);
    REQUIRE(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/master/fx/1/status") == "ok"; }));
    CHECK(tree().getAs<std::string>("/master/fx/1/name") == "Fake Gain");
    CHECK(tree().getAs<std::int64_t>("/master/fx/1/param_count") == 300);
    CHECK(tree().getAs<double>("/master/fx/1/param/p7") == 0.5);
    CHECK(tree().contains("/master/fx/1/param/p127"));
    CHECK_FALSE(tree().contains("/master/fx/1/param/p200"));
    REQUIRE(host.last);
    // gain 2 * 0.25: double
    REQUIRE(client.set({{"/master/fx/1/param/p7", 0.25}}).get().code == ErrorCode::Ok);
    std::this_thread::sleep_for(300ms);
    backend.takeOutputPeak("fake-spk");
    std::this_thread::sleep_for(200ms);
    CHECK(backend.takeOutputPeak("fake-spk") == Approx(0.125).margin(0.02));
    CHECK(client.set({{"/master/fx/1/param/p200", 0.5}}).get().code == ErrorCode::NotFound);
    r = client.call("/master/fx/1/params", {{"offset", std::int64_t{199}}, {"count", std::int64_t{3}}}).get();
    REQUIRE(r.code == ErrorCode::Ok);
    CHECK(std::get<std::string>(r.value).find("\"key\":\"p200\"") != std::string::npos);
    CHECK(tree().contains("/master/fx/1/param/p200")); // delivered before the result
    CHECK(client.set({{"/master/fx/1/param/p200", 0.5}}).get().code == ErrorCode::Ok);
    CHECK(host.last->values[200].load() == 0.5);

    host.last->edit(250, 0.75);
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<double>("/master/fx/1/param/p250") == 0.75; }));
    REQUIRE(client.set({{"/master/fx/1/editor", true}}).get().code == ErrorCode::Ok);
    CHECK(host.last->editor.load());
    host.last->closeWindow();
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<bool>("/master/fx/1/editor") == false; }));

    // scan: progress and the list again; extra folders from the tree
    REQUIRE(client.set({{"/plugins/extra_dirs", std::string("D:/VST3;E:/more")}}).get().code == ErrorCode::Ok);
    REQUIRE(client.call("/plugins/scan").get().code == ErrorCode::Ok);
    CHECK(waitFor([](const proto::ParamTree& t) { return t.getAs<std::string>("/plugins/scan/state") == "idle"; }));
    CHECK(host.scannedDirs == std::vector<std::string>{"D:/VST3", "E:/more"});

    // removing a track drops its effects
    REQUIRE(client.call("/track/2/fx/add", {{"type", std::string("delay")}}).get().code == ErrorCode::Ok);
    REQUIRE(client.call("/mixer/track/remove", {{"track", std::int64_t{2}}}).get().code == ErrorCode::Ok);
    auto added = client.call("/mixer/track/add").get();
    CHECK(std::get<std::int64_t>(added.value) == 2);
    CHECK(tree().pathsUnder("/track/2/fx").empty());
    CHECK(client.call("/track/2/fx/remove", {{"index", std::int64_t{1}}}).get().code == ErrorCode::NotFound);

    client.disconnect();
    ctl.stop();
    server.stop();
    work.reset();
    asio::post(io, [&] { io.stop(); });
    net.join();
    ctlPtr = nullptr;
    devices.stop();
}

namespace {

// An engine (fake devices, the fake plugin host) with profiles in dir, on its own network thread.
struct ProfileRig {
    asio::io_context io;
    proto::ParamStore store;
    Controller* ctlPtr = nullptr;
    ProfileManager* profPtr = nullptr;
    FakeBackend backend;
    Engine engine{nullptr};
    std::unique_ptr<control::ControlServer> server;
    std::unique_ptr<DeviceManager> devices;
    std::unique_ptr<Controller> ctl;
    std::unique_ptr<ProfileManager> prof;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::thread net;

    ProfileRig(FakeHost& host, const std::string& dir, std::string startProfile = {}) {
        control::ControlServer::Options opt;
        opt.port = 0;
        opt.onDelta = [this](const proto::Delta& d) {
            if (ctlPtr) ctlPtr->onDelta(d);
            if (profPtr) profPtr->onDelta(d);
        };
        server = std::make_unique<control::ControlServer>(io, store, opt);
        devices = std::make_unique<DeviceManager>(backend, engine, DeviceManager::Options{});
        Controller::Options co;
        co.verbose = false;
        co.plugins = &host;
        ctl = std::make_unique<Controller>(io, *server, *devices, engine, nullptr, co);
        ProfileManager::Hooks hooks;
        hooks.restoreFx = [this](const auto& slots, const auto& states) { return ctl->fx().restore(slots, states); };
        hooks.restored = [this] { ctl->fx().restored(); };
        hooks.pluginStates = [this] { return ctl->fx().pluginStates(); };
        hooks.deviceNames = [this](const std::vector<std::string>& ids) { return ctl->deviceNames(ids); };
        hooks.rememberDevices = [this](const std::vector<proto::SavedDevice>& d) { ctl->rememberDevices(d); };
        ProfileManager::Options po;
        po.directory = dir;
        po.statePath = dir.empty() ? std::string() : dir + "/mixd-profile.txt";
        po.startProfile = std::move(startProfile);
        po.autosaveDelay = 100ms;
        po.verbose = false;
        prof = std::make_unique<ProfileManager>(io, *server, std::move(hooks), po);
        server->start();
        ctl->start(devices->enumerateNow());
        ctlPtr = ctl.get();
        prof->start();
        profPtr = prof.get();
        devices->start([this](const DeviceManager::State& s) { ctl->onDeviceState(s); });
        work.emplace(asio::make_work_guard(io));
        net = std::thread([this] { io.run(); });
    }
    ~ProfileRig() {
        std::promise<void> done;
        asio::post(io, [&] {
            prof->stop();
            profPtr = nullptr;
            ctl->stop();
            server->stop();
            done.set_value();
        });
        done.get_future().wait();
        work.reset();
        asio::post(io, [&] { io.stop(); });
        net.join();
        ctlPtr = nullptr;
        devices->stop();
    }
    std::uint16_t port() const { return server->port(); }
};

std::string tempDir(const char* name) {
    const auto d = std::filesystem::temp_directory_path() / (std::string("dc-profile-test-") + name);
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d.string();
}

} // namespace

TEST_CASE("profiles: save, restart, switch and snapshots", "[profile][integration]") {
    using proto::ErrorCode;
    const auto dir = tempDir("main");
    FakeHost host;
    auto connect = [](ProfileRig& rig) {
        control::ControlClient::Options o;
        o.port = rig.port();
        o.clientName = "test";
        auto c = std::make_unique<control::ControlClient>(o);
        REQUIRE(c->connect(3s));
        return c;
    };
    auto waitFor = [](control::ControlClient& c, auto pred) {
        for (int i = 0; i < 100; ++i) {
            if (pred(c.tree())) return true;
            std::this_thread::sleep_for(30ms);
        }
        return false;
    };

    {
        ProfileRig rig(host, dir);
        auto client = connect(rig);
        auto tree = [&] { return client->tree(); };
        CHECK(tree().getAs<std::string>("/profile/current") == "標準");
        CHECK(tree().getAs<std::string>("/profile/directory") == dir);
        CHECK_FALSE(tree().contains("/profile/list/1/name")); // saved on the first change

        REQUIRE(client->set({{"/track/1/volume_db", -6.0}, {"/track/1/name", std::string("Mic")},
                             {"/track/1/input/kind", std::string("device")}, {"/track/1/input/source", std::string("fake-line")},
                             {"/capture/video/bitrate_kbps", std::int64_t{40000}}})
                    .get()
                    .code == ErrorCode::Ok);
        REQUIRE(client->call("/mixer/track/add").get().code == ErrorCode::Ok);
        REQUIRE(client->call("/track/1/fx/add", {{"type", std::string("eq4")}}).get().code == ErrorCode::Ok);
        REQUIRE(client->set({{"/track/1/fx/1/param/output_db", -3.0}}).get().code == ErrorCode::Ok);
        REQUIRE(client->call("/master/fx/add", {{"type", std::string("vst3:00000007")}}).get().code == ErrorCode::Ok);
        REQUIRE(waitFor(*client, [](const proto::ParamTree& t) { return t.getAs<std::string>("/master/fx/1/status") == "ok"; }));
        REQUIRE(client->set({{"/master/fx/1/param/p7", 0.25}}).get().code == ErrorCode::Ok);
        host.last->values[280] = 0.9; // only in the plugin's own state

        // a snapshot, then the fader moves on
        auto r = client->call("/snapshot/save", {{"name", std::string("Talk")}}).get();
        REQUIRE(r.code == ErrorCode::Ok);
        CHECK(std::get<std::int64_t>(r.value) == 1);
        CHECK(tree().getAs<std::string>("/snapshot/1/name") == "Talk");
        CHECK(tree().getAs<std::int64_t>("/snapshot/last") == 1);
        REQUIRE(client->set({{"/track/1/volume_db", -20.0}, {"/track/1/name", std::string("Mic 2")}}).get().code == ErrorCode::Ok);
        CHECK(waitFor(*client, [](const proto::ParamTree& t) { return t.getAs<bool>("/profile/modified") == true; }));

        // autosave
        REQUIRE(waitFor(*client, [](const proto::ParamTree& t) {
            return t.getAs<bool>("/profile/modified") == false && t.getAs<std::string>("/profile/list/1/name") == "標準";
        }));
        CHECK(std::filesystem::exists(std::filesystem::path(dir) / std::filesystem::path(u8"標準.dcprofile")));

        // recall: the fader and the effect, not the name
        REQUIRE(client->set({{"/track/1/fx/1/param/output_db", 0.0}}).get().code == ErrorCode::Ok);
        REQUIRE(client->call("/snapshot/recall", {{"snapshot", std::int64_t{1}}}).get().code == ErrorCode::Ok);
        CHECK(tree().getAs<double>("/track/1/volume_db") == -6.0);
        CHECK(tree().getAs<double>("/track/1/fx/1/param/output_db") == -3.0);
        CHECK(tree().getAs<std::string>("/track/1/name") == "Mic 2");
        CHECK(client->call("/snapshot/recall", {{"snapshot", std::int64_t{5}}}).get().code == ErrorCode::NotFound);
        REQUIRE(client->set({{"/track/1/volume_db", -7.5}}).get().code == ErrorCode::Ok);
        client->disconnect();
    } // the engine stops: the last change is saved

    {
        ProfileRig rig(host, dir);
        auto client = connect(rig);
        auto tree = [&] { return client->tree(); };
        CHECK(tree().getAs<std::string>("/profile/current") == "標準");
        CHECK(tree().getAs<double>("/track/1/volume_db") == -7.5);
        CHECK(tree().getAs<std::string>("/track/1/name") == "Mic 2");
        CHECK(tree().getAs<std::string>("/track/1/input/source") == "fake-line");
        CHECK(tree().getAs<std::int64_t>("/mixer/track_count") == 5);
        CHECK(tree().getAs<std::int64_t>("/capture/video/bitrate_kbps") == 40000);
        CHECK(tree().getAs<std::string>("/track/1/fx/1/type") == "eq4");
        CHECK(tree().getAs<double>("/track/1/fx/1/param/output_db") == -3.0);
        CHECK(tree().getAs<std::string>("/snapshot/1/name") == "Talk");
        CHECK(tree().getAs<std::int64_t>("/snapshot/last") == 0);
        REQUIRE(waitFor(*client, [](const proto::ParamTree& t) { return t.getAs<std::string>("/master/fx/1/status") == "ok"; }));
        REQUIRE(host.last);
        CHECK(host.last->restored.load() == 1);
        CHECK(host.last->values[280].load() == Approx(0.9));
        CHECK(tree().getAs<double>("/master/fx/1/param/p7") == Approx(0.25));
        REQUIRE(waitFor(*client, [](const proto::ParamTree& t) { return t.getAs<std::string>("/track/1/input/state") == "ok"; }));

        // a second profile from the fresh state, and back
        CHECK(client->call("/profile/new", {{"name", std::string("a/b")}}).get().code == ErrorCode::BadRequest);
        REQUIRE(client->call("/profile/new", {{"name", std::string("配信")}}).get().code == ErrorCode::Ok);
        CHECK(tree().getAs<std::string>("/profile/current") == "配信");
        CHECK(tree().getAs<std::int64_t>("/mixer/track_count") == 4);
        CHECK(tree().pathsUnder("/master/fx").empty());
        CHECK_FALSE(tree().contains("/snapshot/1/name"));
        CHECK(tree().getAs<std::string>("/profile/list/1/name") == "標準");
        CHECK(tree().getAs<std::string>("/profile/list/2/name") == "配信");
        CHECK(client->call("/profile/new", {{"name", std::string("配信")}}).get().code == ErrorCode::Conflict);
        REQUIRE(client->set({{"/track/2/pan", -0.5}}).get().code == ErrorCode::Ok);

        CHECK(client->call("/profile/load", {{"name", std::string("nope")}}).get().code == ErrorCode::NotFound);
        REQUIRE(client->call("/profile/load", {{"name", std::string("標準")}}).get().code == ErrorCode::Ok);
        CHECK(tree().getAs<double>("/track/1/volume_db") == -7.5);
        CHECK(tree().getAs<std::int64_t>("/mixer/track_count") == 5);
        REQUIRE(waitFor(*client, [](const proto::ParamTree& t) { return t.getAs<std::string>("/master/fx/1/status") == "ok"; }));
        REQUIRE(client->call("/profile/load", {{"name", std::string("配信")}}).get().code == ErrorCode::Ok);
        CHECK(tree().getAs<double>("/track/2/pan") == -0.5); // saved before switching away

        // rename, save as, remove
        CHECK(client->call("/profile/remove", {{"name", std::string("配信")}}).get().code == ErrorCode::Conflict);
        REQUIRE(client->call("/profile/rename", {{"name", std::string("配信")}, {"new_name", std::string("Stream")}}).get().code ==
                ErrorCode::Ok);
        CHECK(tree().getAs<std::string>("/profile/current") == "Stream");
        CHECK(client->call("/profile/rename", {{"name", std::string("Stream")}, {"new_name", std::string("標準")}}).get().code ==
              ErrorCode::Conflict);
        auto r = client->call("/profile/save", {{"name", std::string("Copy")}}).get();
        REQUIRE(r.code == ErrorCode::Ok);
        CHECK(std::get<std::string>(r.value) == "Copy");
        CHECK(tree().getAs<std::string>("/profile/current") == "Copy");
        REQUIRE(client->call("/profile/remove", {{"name", std::string("stream")}}).get().code == ErrorCode::Ok); // any case
        CHECK(tree().getAs<std::string>("/profile/list/1/name") == "Copy");
        CHECK(tree().getAs<std::string>("/profile/list/2/name") == "標準");
        CHECK_FALSE(tree().contains("/profile/list/3/name"));

        // snapshots are kept per profile
        REQUIRE(client->call("/snapshot/save").get().code == ErrorCode::Ok);
        CHECK(tree().getAs<std::string>("/snapshot/1/name") == "Snapshot 1");
        REQUIRE(client->call("/snapshot/remove", {{"snapshot", std::int64_t{1}}}).get().code == ErrorCode::Ok);
        CHECK_FALSE(tree().contains("/snapshot/1/name"));
        CHECK(tree().getAs<std::int64_t>("/snapshot/last") == 0);
        client->disconnect();
    }

    {
        ProfileRig rig(host, dir); // starts with the last one used
        auto client = connect(rig);
        CHECK(client->tree().getAs<std::string>("/profile/current") == "Copy");
        client->disconnect();
    }
    {
        ProfileRig rig(host, dir, "標準"); // --profile
        auto client = connect(rig);
        CHECK(client->tree().getAs<std::string>("/profile/current") == "標準");
        CHECK(client->tree().getAs<std::string>("/snapshot/1/name") == "Talk");
        client->disconnect();
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("profiles: a broken file", "[profile][integration]") {
    const auto dir = tempDir("broken");
    const auto file = std::filesystem::path(dir) / std::filesystem::path(u8"標準.dcprofile");
    FakeHost host;
    {
        std::ofstream(file) << "dcprofile\t1\nv\t/track/1/volume_db\tf\t-3\n"; // cut short
        ProfileRig rig(host, dir);
        control::ControlClient::Options o;
        o.port = rig.port();
        control::ControlClient client(o);
        REQUIRE(client.connect(3s));
        CHECK(client.tree().getAs<std::string>("/profile/current") == "標準");
        CHECK(client.tree().getAs<std::string>("/profile/error").value_or("").find("標準") != std::string::npos);
        CHECK(client.tree().getAs<double>("/track/1/volume_db") == 0.0);
        CHECK(std::filesystem::exists(std::filesystem::path(file.string() + ".broken")));
        client.disconnect();
    }
    {
        // a broken file with a good backup
        std::ofstream(file) << "garbage";
        std::ofstream(std::filesystem::path(file.string() + ".bak")) << "dcprofile\t1\nv\t/track/1/position\ti\t1\n"
                                                                         "v\t/track/1/volume_db\tf\t-3\nend\n";
        ProfileRig rig(host, dir);
        control::ControlClient::Options o;
        o.port = rig.port();
        control::ControlClient client(o);
        REQUIRE(client.connect(3s));
        CHECK(client.tree().getAs<double>("/track/1/volume_db") == -3.0);
        CHECK(client.tree().getAs<std::int64_t>("/mixer/track_count") == 1);
        CHECK(client.tree().getAs<std::string>("/profile/error").value_or("").find(".bak") != std::string::npos);
        client.disconnect();
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("profiles off: snapshots still work", "[profile][integration]") {
    FakeHost host;
    ProfileRig rig(host, "");
    control::ControlClient::Options o;
    o.port = rig.port();
    control::ControlClient client(o);
    REQUIRE(client.connect(3s));
    CHECK(client.tree().getAs<std::string>("/profile/current") == "");
    CHECK(client.call("/profile/save").get().code == proto::ErrorCode::Conflict);
    REQUIRE(client.call("/snapshot/save").get().code == proto::ErrorCode::Ok);
    REQUIRE(client.set({{"/master/volume_db", -10.0}}).get().code == proto::ErrorCode::Ok);
    REQUIRE(client.call("/snapshot/recall", {{"snapshot", std::int64_t{1}}}).get().code == proto::ErrorCode::Ok);
    CHECK(client.tree().getAs<double>("/master/volume_db") == 0.0);
    client.disconnect();
}
