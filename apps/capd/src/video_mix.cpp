#include "video_mix.hpp"

#include "compositor.hpp"
#include "text_renderer.hpp"
#include "video_config.hpp"
#include "video_source.hpp"

#include "dc/shmaudio/clock.hpp"

#include <avrt.h>
#include <mfapi.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <variant>

namespace dc::capd {

std::vector<MonitorInfo> listMonitors() { return enumerateMonitors(); }

namespace {

std::int64_t qpcToMs(std::int64_t qpc) { return qpc * 1000 / shm::qpcFrequency(); }

// Test switch for the G3 disconnect checks (scripts/g3-integration.ps1): with DC_CAPD_TEST_DIR set, a file
// named `name` in that folder is consumed and returns true once. A real GPU reset needs elevation (dxcap
// -forcetdr), so the check goes through the same recovery path from here.
bool takeTestTrigger(const char* name) {
    static const char* dir = std::getenv("DC_CAPD_TEST_DIR");
    if (!dir || !*dir) return false;
    std::error_code ec;
    const std::string d(dir);
    const auto path = std::filesystem::path(std::u8string(d.begin(), d.end())) / name;
    return std::filesystem::remove(path, ec);
}

bool isDeviceLoss(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

// The device lives on the GPU that drives the primary monitor (Desktop Duplication needs that; WGC copies across).
Device createMixerDevice() {
    const auto monitors = enumerateMonitors();
    for (const auto& m : monitors)
        if (m.primary) return createDevice(m.adapter.Get());
    if (!monitors.empty()) return createDevice(monitors.front().adapter.Get());
    ComPtr<IDXGIFactory1> factory;
    DC_HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    DC_HR(factory->EnumAdapters1(0, &adapter));
    return createDevice(adapter.Get());
}

MixSettings mixSettingsOf(const MixConfig& m) {
    MixSettings s;
    s.width = m.width;
    s.height = m.height;
    s.fps = m.fps;
    return s;
}

}  // namespace

// --- shared core: the engine state that leases and the video thread use ---

struct VideoMix::Shared {
    using AnyConsumer = std::variant<VideoMix::FrameConsumer, VideoMix::Consumer>;

    struct Mix {
        // guarded by Shared::mutex
        MixConfig cfg;
        bool exists = false;
        int leases = 0;
        MixSettings settings;
        std::uint64_t generation = 1;
        VideoMix::Stats stats;
        // guarded by consumersMutex (held while the consumers run)
        std::mutex consumersMutex;
        std::map<int, AnyConsumer> consumers;
        // video thread only
        bool running = false;
        std::unique_ptr<Compositor> compositor;
        TransitionTracker tracker;
        TransitionTracker::State transition;
        std::int64_t t0 = 0, index = 0;
        int runW = 0, runH = 0, runFps = 0;
        double secSum = 0, secMax = 0;
        std::uint64_t secTicks = 0;
        std::int64_t secondEnd = 0;
    };

    struct SourceSlot {
        SourceConfig cfg;
        std::unique_ptr<VideoSource> source;
        std::int64_t lastNeededMs = 0;
        std::int64_t retryAtMs = 0;  // after a failure, wait before opening again
        SourceStatus failure;        // shown while waiting
    };

    mutable std::mutex mutex;
    VideoConfig latest;  // last config from update()
    bool dirty = false;
    std::map<int, std::shared_ptr<Mix>> mixes;
    int nextConsumerId = 1;
    std::string error;
    std::string adapterName;
    VideoEngine::Reporter reporter;
    std::map<int, SourceStatus> reported;  // source id -> last status sent
    std::atomic<std::uint64_t> captured{0};
    std::atomic<std::uint64_t> recoveries{0};

    // video thread only (dev is read by leases' device())
    Device dev;
    bool devOk = false;
    std::unique_ptr<TextRenderer> text;
    VideoConfig cfg;  // the video thread's copy
    std::map<int, SourceSlot> sources;
    // DC_CAPD_FRAME_LOG=<file>: one CSV line per tick and capture source, how old the picture on the canvas is
    // (for A/V offset steps: capture latency vs the tick schedule).
    FILE* frameLog = nullptr;

    std::atomic<bool> running{false};
    std::thread thread;

    // device lists
    std::thread watcher;
    std::mutex watchMutex;
    std::condition_variable watchCv;
    bool refreshRequested = true, refreshWindows = true;

    void report(const std::string& method, proto::Args args) {
        VideoEngine::Reporter r;
        {
            std::lock_guard lock(mutex);
            r = reporter;
        }
        if (r) r(method, args);
    }

    void run();
    void pass(std::int64_t now);
    void applyConfig();
    void manageSources(std::int64_t nowMs);
    void tick(Mix& m, std::int64_t index, std::int64_t due);
    std::vector<LayerDraw> layersOf(int scene, const Mix& m);
    void deviceLost(const std::string& why);
    bool recreateDevice();
    void reportSources();
    void watch();
};

void VideoMix::Shared::applyConfig() {
    std::lock_guard lock(mutex);
    if (!dirty) return;
    dirty = false;
    cfg = latest;
    for (auto& [id, m] : mixes) {
        auto it = cfg.mixes.find(id);
        m->exists = it != cfg.mixes.end();
        if (m->exists) {
            m->cfg = it->second;
            m->settings = mixSettingsOf(m->cfg);
        }
    }
}

void VideoMix::Shared::manageSources(std::int64_t nowMs) {
    std::set<int> needed;
    std::vector<std::shared_ptr<Mix>> run;
    {
        std::lock_guard lock(mutex);
        for (auto& [id, m] : mixes)
            if (m->running) run.push_back(m);
    }
    for (auto& m : run) addNeededSources(cfg, m->cfg, m->transition, needed);

    // close sources that were removed, changed, or not needed for a while (3 s, so a quick stop / start of an
    // output does not reopen a camera)
    for (auto it = sources.begin(); it != sources.end();) {
        auto c = cfg.sources.find(it->first);
        const bool gone = c == cfg.sources.end();
        const bool changed = !gone && !(c->second == it->second.cfg);
        if (needed.count(it->first)) it->second.lastNeededMs = nowMs;
        const bool idle = nowMs - it->second.lastNeededMs > 3000;
        if (gone || changed || (idle && !needed.count(it->first))) {
            it = sources.erase(it);
        } else {
            ++it;
        }
    }
    SourceContext ctx{&dev, text.get()};
    for (int id : needed) {
        auto& slot = sources[id];
        if (slot.source) continue;
        if (nowMs < slot.retryAtMs) continue;
        slot.cfg = cfg.sources.at(id);
        slot.lastNeededMs = nowMs;
        slot.failure = {};
        try {
            slot.source = createVideoSource(ctx, slot.cfg);
        } catch (const HrError& e) {
            if (isDeviceLoss(e.code())) throw;
            slot.failure = {"error", e.what(), 0, 0};
            slot.retryAtMs = nowMs + 2000;
        } catch (const std::exception& e) {
            slot.failure = {"error", e.what(), 0, 0};
            slot.retryAtMs = nowMs + 2000;
        }
    }
    // poll once per pass
    for (int id : needed) {
        auto& slot = sources[id];
        if (!slot.source) continue;
        try {
            if (slot.source->poll()) ++captured;
        } catch (const HrError& e) {
            if (isDeviceLoss(e.code()) || FAILED(dev.device->GetDeviceRemovedReason())) throw;
            slot.failure = {"error", e.what(), 0, 0};
            slot.source.reset();
            slot.retryAtMs = nowMs + 2000;
            ++recoveries;
        } catch (const std::exception& e) {
            if (FAILED(dev.device->GetDeviceRemovedReason())) throw;
            slot.failure = {"error", e.what(), 0, 0};
            slot.source.reset();
            slot.retryAtMs = nowMs + 2000;
            ++recoveries;
        }
    }
}

std::vector<LayerDraw> VideoMix::Shared::layersOf(int scene, const Mix& m) {
    std::vector<LayerDraw> out;
    auto sc = cfg.scenes.find(scene);
    if (sc == cfg.scenes.end()) return out;
    const int W = m.runW, H = m.runH;
    for (const auto& l : sc->second.layers) {
        if (!l.visible || l.opacity <= 0) continue;
        auto sit = cfg.sources.find(l.source);
        if (sit == cfg.sources.end()) continue;
        LayerDraw d;
        d.opacity = float(l.opacity);
        LayerGeometry g;
        if (sit->second.kind == "color") {
            d.solid = true;
            d.color = argbToRgba(sit->second.color);
            g = layoutLayer(l, 0, 0, W, H);
        } else {
            auto slot = sources.find(l.source);
            VideoSource* src = slot != sources.end() ? slot->second.source.get() : nullptr;
            ID3D11ShaderResourceView* view = src ? src->view() : nullptr;
            if (view && src->width() > 0 && src->height() > 0) {
                d.view = view;
                d.alpha = src->alpha() == AlphaMode::Opaque ? 0 : src->alpha() == AlphaMode::Straight ? 1 : 2;
                g = layoutLayer(l, src->width(), src->height(), W, H);
                d.chroma = l.chroma;
                d.chromaColor = l.chromaColor;
                d.similarity = float(l.similarity);
                d.smoothness = float(l.smoothness);
                d.spill = float(l.spill);
            } else if (l.noSignalImage && text) {
                d.view = text->noSignalView();
                d.alpha = 2;
                LayerConfig plain = l;
                plain.cropLeft = plain.cropTop = plain.cropRight = plain.cropBottom = 0;
                plain.fit = "fit";
                g = layoutLayer(plain, int(text->noSignalWidth()), int(text->noSignalHeight()), W, H);
            } else {
                continue;  // transparent while the source has no picture
            }
        }
        if (g.empty()) continue;
        d.x = g.x;
        d.y = g.y;
        d.width = g.width;
        d.height = g.height;
        d.u0 = g.u0;
        d.v0 = g.v0;
        d.u1 = g.u1;
        d.v1 = g.v1;
        out.push_back(d);
    }
    return out;
}

void VideoMix::Shared::tick(Mix& m, std::int64_t index, std::int64_t due) {
    if (!m.compositor) m.compositor = std::make_unique<Compositor>(dev, UINT(m.runW), UINT(m.runH));
    m.transition = m.tracker.update(m.cfg, qpcToMs(due));
    const auto& t = m.transition;
    const Rgba bg = argbToRgba(m.cfg.background);
    auto& comp = *m.compositor;
    if (!t.active()) {
        comp.render(Compositor::Target::Canvas, bg, layersOf(t.to, m));
    } else {
        if (t.freezeNow) comp.freeze();
        if (t.fromFrozen) comp.restoreFrozen();
        else comp.render(Compositor::Target::Canvas, bg, layersOf(t.from, m));
        comp.render(Compositor::Target::Scratch, bg, layersOf(t.to, m));
        comp.blendScratchOverCanvas(float(t.progress));
    }

    MixFrame f;
    f.canvas = comp.canvas();
    f.tick = index;
    f.qpc = due;
    f.device = &dev;
    f.width = m.runW;
    f.height = m.runH;
    f.fps = m.runFps;
    {
        std::lock_guard lock(mutex);
        f.generation = m.generation;
    }
    std::lock_guard lock(m.consumersMutex);
    for (auto& [id, c] : m.consumers) {
        if (auto* fc = std::get_if<VideoMix::FrameConsumer>(&c)) (*fc)(f);
        else std::get<VideoMix::Consumer>(c)(f.canvas, f.tick, f.qpc);
    }
    if (frameLog) {
        const double freq = double(shm::qpcFrequency());
        const double lateMs = double(shm::nowQpc() - due) * 1000.0 / freq;
        for (const auto& [sid, slot] : sources) {
            const std::int64_t fq = slot.source ? slot.source->frameQpc() : 0;
            if (!fq) continue;
            std::fprintf(frameLog, "%d,%lld,%lld,%d,%lld,%.3f,%.3f\n", m.cfg.id, static_cast<long long>(index),
                         static_cast<long long>(due), sid, static_cast<long long>(fq), double(due - fq) * 1000.0 / freq,
                         lateMs);
        }
    }
}

void VideoMix::Shared::pass(std::int64_t now) {
    const std::int64_t freq = shm::qpcFrequency();
    applyConfig();

    // start / stop / resize mixes
    std::vector<std::shared_ptr<Mix>> run;
    {
        std::lock_guard lock(mutex);
        for (auto& [id, m] : mixes) {
            const bool want = m->exists && m->leases > 0;
            if (want && (!m->running || m->runW != m->cfg.width || m->runH != m->cfg.height || m->runFps != m->cfg.fps)) {
                if (m->running) ++m->generation;  // the canvas changes: outputs rebuild
                m->running = true;
                m->compositor.reset();
                m->runW = m->cfg.width;
                m->runH = m->cfg.height;
                m->runFps = m->cfg.fps;
                m->t0 = now;
                m->index = 0;
                m->secondEnd = now + freq;
                m->tracker.reset();
                m->transition = {};
                m->transition.to = m->cfg.programScene;
            } else if (!want && m->running) {
                m->running = false;
                m->compositor.reset();
            }
            if (m->running) run.push_back(m);
        }
    }

    manageSources(qpcToMs(now));

    for (auto& m : run) {
        const std::int64_t fps = m->runFps;
        const std::int64_t due = m->t0 + m->index * freq / fps;
        if (due > now) continue;
        if ((now - due) * fps >= freq) {
            // fell behind by at least a frame: skip the missed ticks (they become gaps the outputs fill)
            const std::int64_t missed = (now - due) * fps / freq;
            m->index += missed;
            std::lock_guard lock(mutex);
            m->stats.lateTicks += static_cast<std::uint64_t>(missed);
            continue;
        }
        const std::int64_t w0 = shm::nowQpc();
        tick(*m, m->index, due);
        const double ms = double(shm::nowQpc() - w0) * 1000.0 / double(freq);
        m->secSum += ms;
        m->secMax = std::max(m->secMax, ms);
        ++m->secTicks;
        ++m->index;
        if (shm::nowQpc() >= m->secondEnd) {
            m->secondEnd += freq;
            std::lock_guard lock(mutex);
            m->stats.ticks = static_cast<std::uint64_t>(m->index);
            m->stats.captured = captured;
            m->stats.recoveries = recoveries;
            m->stats.tickMsAvg = m->secTicks ? m->secSum / double(m->secTicks) : 0;
            m->stats.tickMsMax = m->secMax;
            m->secSum = m->secMax = 0;
            m->secTicks = 0;
        }
    }
}

void VideoMix::Shared::deviceLost(const std::string& why) {
    std::cout << "video: GPU lost (" << why << "), recreating" << std::endl;
    sources.clear();
    text.reset();
    {
        std::lock_guard lock(mutex);
        error = "GPU lost: " + why;
        for (auto& [id, m] : mixes) {
            m->compositor.reset();
            ++m->generation;
        }
    }
    devOk = false;
    ++recoveries;
}

bool VideoMix::Shared::recreateDevice() {
    try {
        Device d = createMixerDevice();
        auto t = std::make_unique<TextRenderer>(d);
        dev = d;
        text = std::move(t);
        devOk = true;
        std::lock_guard lock(mutex);
        adapterName = dev.adapterName;
        error.clear();
        std::cout << "video: device on " << dev.adapterName << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex);
        error = std::string("cannot create the GPU device: ") + e.what();
        return false;
    }
}

void VideoMix::Shared::reportSources() {
    std::vector<std::pair<int, SourceStatus>> changed;
    {
        std::lock_guard lock(mutex);
        for (const auto& [id, sc] : cfg.sources) {
            SourceStatus s{"none", "", 0, 0};
            auto it = sources.find(id);
            if (it != sources.end()) {
                if (it->second.source) s = it->second.source->status();
                else if (!it->second.failure.state.empty()) s = it->second.failure;
            }
            auto r = reported.find(id);
            if (r == reported.end() || !(r->second == s)) {
                reported[id] = s;
                changed.push_back({id, s});
            }
        }
        for (auto it = reported.begin(); it != reported.end();)
            it = cfg.sources.count(it->first) ? std::next(it) : reported.erase(it);
    }
    for (const auto& [id, s] : changed) {
        proto::Args a{{"source", std::int64_t{id}}, {"state", s.state}};
        if (s.state == "error") a["error"] = s.error;
        a["width"] = std::int64_t{s.width};
        a["height"] = std::int64_t{s.height};
        report("/video/source/report", std::move(a));
    }
}

void VideoMix::Shared::run() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // MMCSS "Capture" class: keeps the ticks on time when the machine is busy (as OBS does for its video thread).
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    const std::int64_t freq = shm::qpcFrequency();
    std::int64_t nextReport = 0;
    if (const char* path = std::getenv("DC_CAPD_FRAME_LOG"); path && *path) {
        frameLog = std::fopen(path, "w");
        if (frameLog) std::fprintf(frameLog, "mix,tick,due_qpc,source,frame_qpc,age_ms,tick_late_ms\n");
    }

    while (running) {
        if (!devOk) {
            if (!recreateDevice()) {
                Sleep(1000);
                continue;
            }
        }
        const std::int64_t now = shm::nowQpc();
        try {
            pass(now);
        } catch (const HrError& e) {
            if (isDeviceLoss(e.code()) || FAILED(dev.device->GetDeviceRemovedReason())) deviceLost(e.what());
            else {
                std::lock_guard lock(mutex);
                error = e.what();
            }
        } catch (const std::exception& e) {
            if (FAILED(dev.device->GetDeviceRemovedReason())) deviceLost(e.what());
            else {
                std::lock_guard lock(mutex);
                error = e.what();
            }
        }
        if (now >= nextReport) {
            nextReport = now + freq / 4;
            reportSources();
            if (devOk && takeTestTrigger("gpu_reset")) deviceLost("simulated by the test switch");
        }

        // sleep until the next tick of any running mix (at most 10 ms, so new settings apply quickly)
        std::int64_t next = shm::nowQpc() + freq / 100;
        {
            std::lock_guard lock(mutex);
            for (auto& [id, m] : mixes)
                if (m->running && m->runFps > 0) next = std::min(next, m->t0 + m->index * freq / m->runFps);
        }
        const std::int64_t wait = next - shm::nowQpc();
        if (wait > 0 && timer) {
            LARGE_INTEGER rel;
            rel.QuadPart = -static_cast<LONGLONG>(wait * 10'000'000 / freq);
            SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        }
    }
    {
        std::lock_guard lock(mutex);
        for (auto& [id, m] : mixes) {
            m->running = false;
            m->compositor.reset();
        }
    }
    sources.clear();
    text.reset();
    if (frameLog) std::fclose(frameLog);
    frameLog = nullptr;
    if (timer) CloseHandle(timer);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(co)) CoUninitialize();
}

void VideoMix::Shared::watch() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<DisplayInfo> lastDisplays;
    std::vector<CameraInfo> lastCameras;
    bool first = true;
    while (running) {
        bool force = false, windows = false;
        {
            std::unique_lock lock(watchMutex);
            watchCv.wait_for(lock, std::chrono::seconds(3), [&] { return refreshRequested || !running; });
            if (!running) break;
            force = refreshRequested;
            windows = refreshWindows;
            refreshRequested = refreshWindows = false;
        }
        try {
            const auto displays = listDisplays();
            if (force || first || displays != lastDisplays) {
                proto::Args a{{"kind", std::string("display")}};
                for (std::size_t i = 0; i < displays.size(); ++i) {
                    const std::string k = std::to_string(i + 1) + "/";
                    a[k + "id"] = displays[i].id;
                    a[k + "name"] = displays[i].name;
                    a[k + "width"] = std::int64_t{displays[i].width};
                    a[k + "height"] = std::int64_t{displays[i].height};
                    a[k + "primary"] = displays[i].primary;
                }
                report("/video/devices/report", std::move(a));
                lastDisplays = displays;
            }
            const auto cameras = listCameras();
            if (force || first || cameras != lastCameras) {
                proto::Args a{{"kind", std::string("camera")}};
                for (std::size_t i = 0; i < cameras.size(); ++i) {
                    const std::string k = std::to_string(i + 1) + "/";
                    a[k + "id"] = cameras[i].id;
                    a[k + "name"] = cameras[i].name;
                }
                report("/video/devices/report", std::move(a));
                lastCameras = cameras;
            }
            if (windows || first) {
                const auto list = listWindows();
                proto::Args a{{"kind", std::string("window")}};
                for (std::size_t i = 0; i < list.size() && i < 200; ++i) {
                    const std::string k = std::to_string(i + 1) + "/";
                    a[k + "exe"] = list[i].exe;
                    a[k + "title"] = list[i].title;
                }
                report("/video/devices/report", std::move(a));
            }
            first = false;
        } catch (const std::exception& e) {
            std::cout << "video: device list failed: " << e.what() << std::endl;
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

// --- VideoMix (lease) ---

VideoMix::VideoMix(std::shared_ptr<Shared> shared, int mix) : shared_(std::move(shared)), mix_(mix) {}

VideoMix::~VideoMix() {
    for (int id : consumerIds_) removeConsumer(id);
    std::lock_guard lock(shared_->mutex);
    if (auto it = shared_->mixes.find(mix_); it != shared_->mixes.end()) --it->second->leases;
}

const Device& VideoMix::device() const { return shared_->dev; }

const MixSettings& VideoMix::settings() const {
    std::lock_guard lock(shared_->mutex);
    return shared_->mixes.at(mix_)->settings;
}

std::uint64_t VideoMix::generation() const {
    std::lock_guard lock(shared_->mutex);
    return shared_->mixes.at(mix_)->generation;
}

std::string VideoMix::sourceDescription() const {
    std::lock_guard lock(shared_->mutex);
    const auto& m = *shared_->mixes.at(mix_);
    std::string s = "mix " + std::to_string(mix_) + " \"" + m.cfg.name + "\" scene " + std::to_string(m.cfg.programScene);
    auto sc = shared_->latest.scenes.find(m.cfg.programScene);
    if (sc != shared_->latest.scenes.end()) {
        s += " (";
        bool firstLayer = true;
        for (auto it = sc->second.layers.rbegin(); it != sc->second.layers.rend(); ++it) {
            auto src = shared_->latest.sources.find(it->source);
            if (src == shared_->latest.sources.end()) continue;
            s += (firstLayer ? "" : ", ") + src->second.kind;
            firstLayer = false;
        }
        s += ")";
    }
    return s + " on " + shared_->adapterName;
}

int VideoMix::addFrameConsumer(FrameConsumer c) {
    std::shared_ptr<Shared::Mix> m;
    int id;
    {
        std::lock_guard lock(shared_->mutex);
        m = shared_->mixes.at(mix_);
        id = shared_->nextConsumerId++;
    }
    std::lock_guard lock(m->consumersMutex);
    m->consumers[id] = std::move(c);
    consumerIds_.push_back(id);
    return id;
}

int VideoMix::addConsumer(Consumer c) {
    std::shared_ptr<Shared::Mix> m;
    int id;
    {
        std::lock_guard lock(shared_->mutex);
        m = shared_->mixes.at(mix_);
        id = shared_->nextConsumerId++;
    }
    std::lock_guard lock(m->consumersMutex);
    m->consumers[id] = std::move(c);
    consumerIds_.push_back(id);
    return id;
}

void VideoMix::removeConsumer(int id) {
    std::shared_ptr<Shared::Mix> m;
    {
        std::lock_guard lock(shared_->mutex);
        m = shared_->mixes.at(mix_);
    }
    std::lock_guard lock(m->consumersMutex);
    m->consumers.erase(id);
    consumerIds_.erase(std::remove(consumerIds_.begin(), consumerIds_.end(), id), consumerIds_.end());
}

VideoMix::Stats VideoMix::stats() const {
    std::lock_guard lock(shared_->mutex);
    return shared_->mixes.at(mix_)->stats;
}

std::string VideoMix::error() const {
    std::lock_guard lock(shared_->mutex);
    return shared_->error;
}

// --- VideoEngine ---

struct VideoEngine::Impl {
    std::shared_ptr<VideoMix::Shared> shared = std::make_shared<VideoMix::Shared>();
    bool mfStarted = false;
};

VideoEngine::VideoEngine() : impl_(std::make_unique<Impl>()) {
    impl_->mfStarted = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    auto& s = *impl_->shared;
    // created here so that outputs can open their encoders on device() right after acquire()
    s.dev = createMixerDevice();
    s.text = std::make_unique<TextRenderer>(s.dev);
    s.devOk = true;
    s.adapterName = s.dev.adapterName;
    s.running = true;
    s.thread = std::thread([&s] { s.run(); });
    s.watcher = std::thread([&s] { s.watch(); });
}

VideoEngine::~VideoEngine() {
    auto& s = *impl_->shared;
    s.running = false;
    s.watchCv.notify_all();
    if (s.watcher.joinable()) s.watcher.join();
    if (s.thread.joinable()) s.thread.join();
    if (impl_->mfStarted) MFShutdown();
}

void VideoEngine::update(const proto::ParamTree& tree) {
    auto cfg = videoConfigFromTree(tree);
    auto& s = *impl_->shared;
    std::lock_guard lock(s.mutex);
    if (cfg == s.latest) return;
    s.latest = std::move(cfg);
    s.dirty = true;
    // mixes that exist get a runtime entry; their settings are readable before the video thread applies them
    for (const auto& [id, m] : s.latest.mixes) {
        auto& rt = s.mixes[id];
        if (!rt) rt = std::make_shared<VideoMix::Shared::Mix>();
        if (!rt->running) {
            rt->cfg = m;
            rt->settings = mixSettingsOf(m);
        }
    }
}

std::shared_ptr<VideoMix> VideoEngine::acquire(int mix) {
    auto& s = *impl_->shared;
    std::lock_guard lock(s.mutex);
    auto cfg = s.latest.mixes.find(mix);
    if (cfg == s.latest.mixes.end()) return nullptr;
    auto& rt = s.mixes[mix];
    if (!rt) rt = std::make_shared<VideoMix::Shared::Mix>();
    rt->exists = true;
    if (!rt->running) {
        rt->cfg = cfg->second;
        rt->settings = mixSettingsOf(cfg->second);
    }
    ++rt->leases;
    s.dirty = true;  // let the video thread pick up the lease with the current config
    return std::shared_ptr<VideoMix>(new VideoMix(impl_->shared, mix));
}

void VideoEngine::setReporter(Reporter r) {
    auto& s = *impl_->shared;
    std::lock_guard lock(s.mutex);
    s.reporter = std::move(r);
}

void VideoEngine::refreshDeviceLists(bool withWindows) {
    auto& s = *impl_->shared;
    {
        std::lock_guard lock(s.watchMutex);
        s.refreshRequested = true;
        s.refreshWindows = s.refreshWindows || withWindows;
    }
    s.watchCv.notify_all();
}

void VideoEngine::reportAll() {
    auto& s = *impl_->shared;
    {
        std::lock_guard lock(s.mutex);
        s.reported.clear();
    }
    refreshDeviceLists(true);
}

std::string VideoEngine::adapterName() const {
    std::lock_guard lock(impl_->shared->mutex);
    return impl_->shared->adapterName;
}

}  // namespace dc::capd
