// mixd's VST3 host (vst3_host.cpp) with the DC Test Gain fixture: scan in child processes, the
// cache, loading, parameters and audio. The plugin window is left to the Windows real-machine test.

#include "vst3_host.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace dc::mixd;
using Catch::Matchers::WithinAbs;

namespace {

std::string g_pluginDir;
std::string g_self;

std::string tempCache(const char* name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove(p);
    return p.string();
}

std::unique_ptr<PluginHost> makeTestHost(const std::string& cache) {
    vst3::HostOptions o;
    o.cacheFile = cache;
    o.scannerExe = g_self;
    o.scanTimeoutSeconds = 30;
    o.verbose = false;
    o.standardDirs = false; // only the fixture: the PC's own plugins take a minute to scan
    return vst3::makeHost(o);
}

// Runs a scan of the fixture's directory and waits for it.
void scanAndWait(PluginHost& host) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    REQUIRE(host.scan(true, {g_pluginDir}, [](PluginHost::ScanProgress) {}, [&] {
        std::lock_guard lock(m);
        done = true;
        cv.notify_all();
    }));
    std::unique_lock lock(m);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(90), [&] { return done; }));
}

std::optional<PluginInfo> findTestGain(const std::vector<PluginInfo>& list) {
    for (const auto& p : list)
        if (p.name == "DC Test Gain") return p;
    return std::nullopt;
}

std::shared_ptr<PluginInstance> createAndWait(PluginHost& host, const std::string& id, std::string* error = nullptr) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::shared_ptr<PluginInstance> out;
    host.create(id, [&](std::shared_ptr<PluginInstance> inst, std::string err) {
        std::lock_guard lock(m);
        out = std::move(inst);
        if (error) *error = std::move(err);
        done = true;
        cv.notify_all();
    });
    std::unique_lock lock(m);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(30), [&] { return done; }));
    return out;
}

float peak(const std::vector<float>& v) {
    float m = 0;
    for (float s : v) m = std::max(m, std::abs(s));
    return m;
}

// Runs a 1 kHz sine at 0.5 through the plugin for a few blocks and returns the last block's peak.
float runSine(PluginInstance& inst, int channels) {
    std::vector<float> l(256), r(256);
    float* x[2] = {l.data(), r.data()};
    double phase = 0;
    for (int block = 0; block < 20; ++block) {
        for (int i = 0; i < 256; ++i) {
            l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] = 0.5f * static_cast<float>(std::sin(phase));
            phase += 2 * 3.14159265358979 * 1000.0 / 48000.0;
        }
        inst.process(x, channels, 256);
    }
    return peak(l);
}

} // namespace

TEST_CASE("scan finds the fixture in a child process and caches it", "[vst3]") {
    const auto cache = tempCache("dc-vst3-test-cache.json");
    {
        auto host = makeTestHost(cache);
        scanAndWait(*host);
        const auto p = findTestGain(host->plugins());
        REQUIRE(p.has_value());
        CHECK(p->status == "ok");
        CHECK(p->vendor == "DesktopCapture");
        CHECK(p->id.rfind("vst3:", 0) == 0);
        CHECK_FALSE(p->instrument);
    }
    REQUIRE(std::filesystem::exists(cache));
    // a new host starts from the cache without scanning
    auto again = makeTestHost(cache);
    REQUIRE(findTestGain(again->plugins()).has_value());
}

TEST_CASE("the fixture loads, lists its parameters and processes audio", "[vst3]") {
    auto host = makeTestHost(tempCache("dc-vst3-test-cache2.json"));
    scanAndWait(*host); // once per section (Catch2 reruns the case), quick with only the fixture
    const auto info = findTestGain(host->plugins());
    REQUIRE(info.has_value());

    auto inst = createAndWait(*host, info->id);
    REQUIRE(inst != nullptr);
    CHECK(inst->name() == "DC Test Gain");
    CHECK(inst->hasEditor());
    REQUIRE(inst->paramCount() == 4); // Gain, Mute, Mode, Steps (no bypass)

    const auto params = inst->params(0, 100);
    REQUIRE(params.size() == 4);
    CHECK(params[0].name == "Gain");
    CHECK(params[0].unit == "dB");
    CHECK(params[0].key.rfind("p", 0) == 0);
    CHECK_THAT(params[0].value, WithinAbs(60.0 / 72.0, 1e-3)); // 0 dB of -60..12
    CHECK(params[2].steps == 3);
    REQUIRE(inst->param(params[0].key).has_value());
    CHECK_FALSE(inst->param("p999999").has_value());

    SECTION("stereo at 0 dB passes through") { CHECK_THAT(runSine(*inst, 2), WithinAbs(0.5, 0.01)); }
    SECTION("mono strips are fed both channels") { CHECK_THAT(runSine(*inst, 1), WithinAbs(0.5, 0.01)); }
    SECTION("a parameter set reaches the processor") {
        inst->setParam(params[0].key, 0.0); // -60 dB
        CHECK(runSine(*inst, 2) < 0.002f);
        const auto g = inst->param(params[0].key);
        REQUIRE(g.has_value());
        CHECK_THAT(g->value, WithinAbs(0.0, 1e-6));
        CHECK(g->text.find("-60") != std::string::npos);
    }
    SECTION("mute") {
        inst->setParam(params[1].key, 1.0);
        CHECK(runSine(*inst, 2) < 1e-6f);
    }
}

TEST_CASE("an unknown plugin id fails cleanly", "[vst3]") {
    auto host = makeTestHost(tempCache("dc-vst3-test-cache3.json"));
    std::string error;
    auto inst = createAndWait(*host, "vst3:00000000", &error);
    CHECK(inst == nullptr);
    CHECK_FALSE(error.empty());
}

// Run with DC_TEST_PLUGIN_CRASH=1: the plugin crashes while it is created, which must only take the
// scanner child down.
TEST_CASE("a plugin that crashes in the scanner is marked, the host survives", "[crash]") {
    auto host = makeTestHost(tempCache("dc-vst3-test-cache4.json"));
    scanAndWait(*host);
    bool sawFailure = false;
    for (const auto& p : host->plugins())
        if (p.path.find("DC Test Gain") != std::string::npos) {
            CHECK(p.status != "ok");
            sawFailure = true;
        }
    CHECK(sawFailure);
}

int main(int argc, char** argv) {
    g_self = std::filesystem::absolute(argv[0]).string();
    std::vector<char*> rest;
    std::string scanFile, scanOut;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--plugin-dir" && i + 1 < argc) g_pluginDir = argv[++i];
        else if (a == "--scan-plugin" && i + 1 < argc) scanFile = argv[++i];
        else if (a == "--out" && i + 1 < argc) scanOut = argv[++i];
        else rest.push_back(argv[i]);
    }
    if (!scanFile.empty()) return vst3::scanChildMain(scanFile, scanOut);
    if (g_pluginDir.empty()) {
        std::fprintf(stderr, "usage: dc_vst3_tests --plugin-dir DIR [catch2 options]\n");
        return 2;
    }
    return vst3::runWithMessageLoop([&] { return Catch::Session().run(static_cast<int>(rest.size()), rest.data()); });
}
