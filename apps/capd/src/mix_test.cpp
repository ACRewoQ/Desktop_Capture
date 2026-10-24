// "capd mixtest": checks the video mixer on a real PC without the engine. Builds trees with the same functions the
// engine uses (dc/protocol/video.hpp), runs them through a VideoEngine, reads the canvas back and checks pixels:
// colours and stacking, fit / fill / crop, opacity, chroma key, text, fade and cut, then monitor, window, camera and
// video file sources, four mixes at once and a canvas size change. Every checked picture is saved as PNG.

#include "mix_test.hpp"

#include "text_renderer.hpp"
#include "video_mix.hpp"
#include "video_source.hpp"

#include "dc/protocol/defaults.hpp"
#include "dc/protocol/param_store.hpp"
#include "dc/protocol/video.hpp"

#include <mfapi.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

namespace dc::capd {

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

struct Picture {
    int width = 0, height = 0;
    std::vector<std::uint8_t> bgra;  // premultiplied, top-down
    struct Px {
        int r, g, b, a;
    };
    Px at(double fx, double fy) const {
        const int x = std::clamp(int(fx * width), 0, width - 1), y = std::clamp(int(fy * height), 0, height - 1);
        const auto* p = &bgra[(size_t(y) * size_t(width) + size_t(x)) * 4];
        return {p[2], p[1], p[0], p[3]};
    }
};

// Copies the canvas of the next tick to the CPU.
class Grabber {
public:
    explicit Grabber(VideoMix& mix) : mix_(mix) {
        id_ = mix_.addFrameConsumer([this](const MixFrame& f) { onFrame(f); });
    }
    ~Grabber() { mix_.removeConsumer(id_); }

    // Waits for a frame produced at least `after` from now.
    Picture grab(std::chrono::milliseconds after = 0ms) {
        if (after.count() > 0) std::this_thread::sleep_for(after);
        std::unique_lock lock(m_);
        want_ = true;
        ready_ = false;
        if (!cv_.wait_for(lock, 3s, [&] { return ready_; })) throw std::runtime_error("no frame from the mix in 3 s");
        return pic_;
    }
    std::uint64_t generation() {
        std::lock_guard lock(m_);
        return generation_;
    }
    std::uint64_t frames() {
        std::lock_guard lock(m_);
        return frames_;
    }

private:
    void onFrame(const MixFrame& f) {
        std::lock_guard lock(m_);
        generation_ = f.generation;
        ++frames_;
        if (!want_) return;
        want_ = false;
        auto* dev = f.device;
        D3D11_TEXTURE2D_DESC desc{};
        f.canvas->GetDesc(&desc);
        if (!staging_ || stW_ != desc.Width || stH_ != desc.Height || stGen_ != f.generation) {
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            staging_.Reset();
            DC_HR(dev->device->CreateTexture2D(&desc, nullptr, &staging_));
            stW_ = desc.Width;
            stH_ = desc.Height;
            stGen_ = f.generation;
        }
        dev->context->CopyResource(staging_.Get(), f.canvas);
        D3D11_MAPPED_SUBRESOURCE m{};
        DC_HR(dev->context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m));
        pic_.width = int(stW_);
        pic_.height = int(stH_);
        pic_.bgra.resize(size_t(stW_) * stH_ * 4);
        for (UINT y = 0; y < stH_; ++y)
            std::memcpy(&pic_.bgra[size_t(y) * stW_ * 4], static_cast<const std::uint8_t*>(m.pData) + size_t(y) * m.RowPitch,
                        size_t(stW_) * 4);
        dev->context->Unmap(staging_.Get(), 0);
        ready_ = true;
        cv_.notify_all();
    }

    VideoMix& mix_;
    int id_ = 0;
    std::mutex m_;
    std::condition_variable cv_;
    bool want_ = false, ready_ = false;
    Picture pic_;
    ComPtr<ID3D11Texture2D> staging_;
    UINT stW_ = 0, stH_ = 0;
    std::uint64_t stGen_ = 0, generation_ = 0, frames_ = 0;
};

void savePng(const Picture& p, const fs::path& file) {
    ComPtr<IWICImagingFactory> wic;
    DC_HR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
    ComPtr<IWICStream> stream;
    DC_HR(wic->CreateStream(&stream));
    DC_HR(stream->InitializeFromFilename(file.wstring().c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> enc;
    DC_HR(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc));
    DC_HR(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    DC_HR(enc->CreateNewFrame(&frame, nullptr));
    DC_HR(frame->Initialize(nullptr));
    DC_HR(frame->SetSize(UINT(p.width), UINT(p.height)));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
    DC_HR(frame->SetPixelFormat(&fmt));
    DC_HR(frame->WritePixels(UINT(p.height), UINT(p.width) * 4, UINT(p.bgra.size()),
                             const_cast<BYTE*>(p.bgra.data())));
    DC_HR(frame->Commit());
    DC_HR(enc->Commit());
}

// 400x200: left half pure green, right half magenta.
void writeTestImage(const fs::path& file) {
    Picture p;
    p.width = 400;
    p.height = 200;
    p.bgra.resize(400 * 200 * 4);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 400; ++x) {
            auto* px = &p.bgra[(size_t(y) * 400 + size_t(x)) * 4];
            const bool left = x < 200;
            px[0] = left ? 0 : 255;    // B
            px[1] = left ? 255 : 0;    // G
            px[2] = left ? 0 : 255;    // R
            px[3] = 255;
        }
    savePng(p, file);
}

class Test {
public:
    Test(fs::path out, std::string media) : out_(std::move(out)), media_(std::move(media)) {}

    int run();

private:
    // --- tree helpers ---
    std::int64_t call(const std::string& method, proto::Args args = {}) {
        auto o = proto::videoCall(store_.tree(), method, args, proto::ClientRole::Local);
        if (o.result.code != proto::ErrorCode::Ok) throw std::runtime_error(method + ": " + o.result.message);
        store_.publish(o.changes, o.removed);
        return std::holds_alternative<std::int64_t>(o.result.value) ? std::get<std::int64_t>(o.result.value) : 0;
    }
    void set(std::vector<proto::Change> c) {
        store_.publish(c);
    }
    void push() { engine_->update(store_.tree()); }
    std::string base(std::int64_t scene, std::int64_t layer) {
        return "/video/scene/" + std::to_string(scene) + "/layer/" + std::to_string(layer);
    }
    // A fresh scene shown by mix 1 with a cut.
    std::int64_t newScene() {
        const auto s = call("/video/scene/add");
        set({{"/video/mix/1/transition/type", std::string("cut")}, {"/video/mix/1/program_scene", s}});
        return s;
    }
    std::int64_t colorSource(std::uint32_t argb) {
        const auto id = call("/video/source/add", {{"kind", std::string("color")}});
        set({{"/video/source/" + std::to_string(id) + "/color/value", std::int64_t{argb}}});
        return id;
    }
    std::int64_t layer(std::int64_t scene, std::int64_t source) {
        return call("/video/scene/" + std::to_string(scene) + "/layer/add", {{"source", source}});
    }

    // --- checks ---
    bool like(const Picture::Px& p, int r, int g, int b, int tol = 12) {
        return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol && std::abs(p.b - b) <= tol;
    }
    void check(const std::string& name, bool ok, const std::string& detail = {}) {
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << name << (detail.empty() ? "" : "  (" + detail + ")") << std::endl;
        results_.push_back({name, ok ? "PASS" : "FAIL", detail});
        if (!ok) ++failures_;
    }
    void note(const std::string& name, const std::string& what) {
        std::cout << "  NOTE  " << name << "  (" << what << ")" << std::endl;
        results_.push_back({name, "NOTE", what});
    }
    static std::string px(const Picture::Px& p) {
        char b[48];
        std::snprintf(b, sizeof b, "rgb %d,%d,%d a %d", p.r, p.g, p.b, p.a);
        return b;
    }
    Picture shot(const std::string& name, std::chrono::milliseconds after = 150ms) {
        push();
        auto p = grabber_->grab(after);
        savePng(p, out_ / (name + ".png"));
        return p;
    }
    // Waits until capd's own status for a source becomes `state` (through a reporter, as the engine would see it).
    std::string waitState(std::int64_t source, const std::string& state, std::chrono::milliseconds timeout) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            {
                std::lock_guard lock(statusMutex_);
                auto it = status_.find(int(source));
                if (it != status_.end() && it->second.first == state) return it->second.first;
                if (std::chrono::steady_clock::now() > end) return it == status_.end() ? "none" : it->second.first + " " + it->second.second;
            }
            std::this_thread::sleep_for(100ms);
        }
    }

    void basics();
    void transitions();
    void captures();
    void fourMixes();
    void resize();

    fs::path out_;
    std::string media_;
    proto::ParamStore store_;
    std::unique_ptr<VideoEngine> engine_;
    std::shared_ptr<VideoMix> mix_;
    std::unique_ptr<Grabber> grabber_;
    std::mutex statusMutex_;
    std::map<int, std::pair<std::string, std::string>> status_;  // source -> state, error
    struct Row {
        std::string name, result, detail;
    };
    std::vector<Row> results_;
    int failures_ = 0;
};

void Test::basics() {
    std::cout << "\n[basics]" << std::endl;
    const auto red = colorSource(0xFFFF0000), blue = colorSource(0xFF0000FF), white = colorSource(0xFFFFFFFF);

    // stacking and boxes
    auto s = newScene();
    layer(s, red);
    const auto lb = layer(s, blue);  // in front
    set({{base(s, lb) + "/x", 0.5}, {base(s, lb) + "/y", 0.5}, {base(s, lb) + "/width", 0.25}, {base(s, lb) + "/height", 0.25}});
    auto p = shot("01-stacking");
    check("colour layer fills the canvas", like(p.at(0.2, 0.2), 255, 0, 0), px(p.at(0.2, 0.2)));
    check("front layer in its box", like(p.at(0.6, 0.6), 0, 0, 255), px(p.at(0.6, 0.6)));
    check("outside the box", like(p.at(0.8, 0.8), 255, 0, 0), px(p.at(0.8, 0.8)));
    call("/video/scene/" + std::to_string(s) + "/layer/move", {{"layer", lb}, {"position", std::int64_t{2}}});
    p = shot("02-reordered");
    check("moved behind: hidden", like(p.at(0.6, 0.6), 255, 0, 0), px(p.at(0.6, 0.6)));

    // opacity
    s = newScene();
    const auto lw = layer(s, white);
    set({{base(s, lw) + "/opacity", 0.5}});
    p = shot("03-opacity");
    check("opacity 0.5 over black", like(p.at(0.5, 0.5), 128, 128, 128, 6), px(p.at(0.5, 0.5)));

    // image: fit, crop, fill
    const auto imgFile = out_ / "test-image.png";
    writeTestImage(imgFile);
    const auto img = call("/video/source/add", {{"kind", std::string("image")}});
    set({{"/video/source/" + std::to_string(img) + "/file", imgFile.string()}});
    s = newScene();
    const auto li = layer(s, img);
    p = shot("04-image-fit", 600ms);  // decoded on a worker thread
    check("image fit: letterbox above", like(p.at(0.5, 0.02), 0, 0, 0), px(p.at(0.5, 0.02)));
    check("image fit: left half green", like(p.at(0.25, 0.5), 0, 255, 0), px(p.at(0.25, 0.5)));
    check("image fit: right half magenta", like(p.at(0.75, 0.5), 255, 0, 255), px(p.at(0.75, 0.5)));
    set({{base(s, li) + "/crop/right", 0.5}});
    p = shot("05-image-crop");
    check("crop right half: pillarbox", like(p.at(0.1, 0.5), 0, 0, 0), px(p.at(0.1, 0.5)));
    check("crop right half: only green", like(p.at(0.7, 0.5), 0, 255, 0), px(p.at(0.7, 0.5)));
    set({{base(s, li) + "/crop/right", 0.0}, {base(s, li) + "/fit", std::string("fill")}, {base(s, li) + "/width", 0.25}});
    p = shot("06-image-fill");
    // box 480x1080 (portrait): fill shows the middle 22.2% of the image width: the green/magenta edge in the middle
    check("fill: left of the box centre green", like(p.at(0.08, 0.5), 0, 255, 0), px(p.at(0.08, 0.5)));
    check("fill: right of the box centre magenta", like(p.at(0.17, 0.5), 255, 0, 255), px(p.at(0.17, 0.5)));
    check("fill: outside the box", like(p.at(0.5, 0.5), 0, 0, 0), px(p.at(0.5, 0.5)));

    // chroma key: the green half disappears over blue
    s = newScene();
    layer(s, blue);
    const auto lk = layer(s, img);
    set({{base(s, lk) + "/chroma/enabled", true}, {base(s, lk) + "/fit", std::string("stretch")}});
    p = shot("07-chroma");
    check("chroma: green keyed out (blue behind)", like(p.at(0.25, 0.5), 0, 0, 255, 20), px(p.at(0.25, 0.5)));
    check("chroma: magenta kept", like(p.at(0.75, 0.5), 255, 0, 255, 20), px(p.at(0.75, 0.5)));

    // text
    const auto txt = call("/video/source/add", {{"kind", std::string("text")}});
    set({{"/video/source/" + std::to_string(txt) + "/text/content", std::string("ビデオミキサー TEST")},
         {"/video/source/" + std::to_string(txt) + "/text/size", std::int64_t{120}},
         {"/video/source/" + std::to_string(txt) + "/text/background", std::int64_t{0xFF0000FF}}});
    s = newScene();
    layer(s, txt);
    p = shot("08-text", 300ms);
    check("text drawn on its background", like(p.at(0.02, 0.5), 0, 0, 255) || like(p.at(0.5, 0.5), 0, 0, 255),
          px(p.at(0.5, 0.5)));
    int whiteish = 0;
    for (int i = 0; i < 200; ++i) {
        const auto q = p.at(0.05 + 0.9 * i / 200.0, 0.5);
        if (q.r > 200 && q.g > 200 && q.b > 200) ++whiteish;
    }
    check("text has white glyph pixels", whiteish > 5, std::to_string(whiteish) + " of 200 samples");

    // no signal picture
    const auto missing = call("/video/source/add", {{"kind", std::string("image")}});
    set({{"/video/source/" + std::to_string(missing) + "/file", std::string("C:\\no\\such\\file.png")}});
    s = newScene();
    const auto ln = layer(s, missing);
    set({{base(s, ln) + "/no_signal", std::string("image")}});
    p = shot("09-no-signal", 400ms);
    check("missing file reports an error", waitState(missing, "error", 3s) == "error");
    check("no-signal picture shown", !like(p.at(0.5, 0.5), 0, 0, 0, 4), px(p.at(0.5, 0.5)));
}

void Test::transitions() {
    std::cout << "\n[transitions]" << std::endl;
    const auto red = colorSource(0xFFFF0000), blue = colorSource(0xFF0000FF);
    const auto a = newScene();
    layer(a, red);
    const auto b = call("/video/scene/add");
    layer(b, blue);
    shot("10-scene-a");
    set({{"/video/mix/1/transition/type", std::string("fade")},
         {"/video/mix/1/transition/duration_ms", std::int64_t{1000}},
         {"/video/mix/1/preview_scene", b}});
    call("/video/mix/1/take");
    auto p = shot("11-fade-half", 500ms);
    const auto c = p.at(0.5, 0.5);
    check("fade halfway mixes both scenes", c.r > 60 && c.r < 200 && c.b > 60 && c.b < 200, px(c));
    p = shot("12-fade-done", 700ms);
    check("fade ends on the new scene", like(p.at(0.5, 0.5), 0, 0, 255), px(p.at(0.5, 0.5)));
    check("take swapped preview", store_.tree().getAs<std::int64_t>("/video/mix/1/preview_scene") == a);
    // interrupted: back to A halfway, then the fade continues from the frozen picture
    call("/video/mix/1/take");
    std::this_thread::sleep_for(400ms);
    call("/video/mix/1/take");
    p = shot("13-fade-interrupted", 1300ms);
    check("interrupted fade ends on the last scene", like(p.at(0.5, 0.5), 0, 0, 255), px(p.at(0.5, 0.5)));
    set({{"/video/mix/1/transition/type", std::string("cut")}, {"/video/mix/1/program_scene", a}});
    p = shot("14-cut", 100ms);
    check("cut switches at once", like(p.at(0.5, 0.5), 255, 0, 0), px(p.at(0.5, 0.5)));
}

void Test::captures() {
    std::cout << "\n[sources]" << std::endl;
    // monitor
    const auto disp = call("/video/source/add", {{"kind", std::string("display")}});
    auto s = newScene();
    layer(s, disp);
    push();
    check("primary monitor (wgc) opens", waitState(disp, "ok", 5s) == "ok");
    shot("20-display-wgc", 300ms);
    set({{"/video/source/" + std::to_string(disp) + "/display/method", std::string("dda")}});
    push();
    const auto dda = waitState(disp, "ok", 5s);
    if (dda == "ok") check("primary monitor (dda) opens", true);
    else note("primary monitor (dda)", dda);
    shot("21-display-dda", 300ms);

    // window: Notepad
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    wchar_t cmd[] = L"notepad.exe";
    std::vector<HWND> notepadsBefore;  // the user's own Notepad windows stay open
    for (const auto& w : listWindows())
        if (_stricmp(w.exe.c_str(), "notepad.exe") == 0) notepadsBefore.push_back(w.hwnd);
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::this_thread::sleep_for(2000ms);
        const auto win = call("/video/source/add", {{"kind", std::string("window")}});
        set({{"/video/source/" + std::to_string(win) + "/window/exe", std::string("notepad.exe")},
             {"/video/source/" + std::to_string(win) + "/window/match", std::string("exe")}});
        s = newScene();
        layer(s, win);
        push();
        check("window (Notepad) opens", waitState(win, "ok", 6s) == "ok");
        shot("22-window", 300ms);
        // Win11's Notepad is a packaged app: the started process may hand over, so close the Notepad windows that
        // appeared since the start
        for (const auto& w : listWindows())
            if (_stricmp(w.exe.c_str(), "notepad.exe") == 0 &&
                std::find(notepadsBefore.begin(), notepadsBefore.end(), w.hwnd) == notepadsBefore.end())
                PostMessageW(w.hwnd, WM_CLOSE, 0, 0);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        check("closed window reports disconnected", waitState(win, "disconnected", 6s) == "disconnected");
    } else {
        note("window", "could not start notepad.exe");
    }

    // camera
    std::vector<CameraInfo> cams = listCameras();
    if (cams.empty()) {
        note("camera", "no camera on this PC");
    } else {
        const auto cam = call("/video/source/add", {{"kind", std::string("camera")}});
        set({{"/video/source/" + std::to_string(cam) + "/camera/id", cams.front().id},
             {"/video/source/" + std::to_string(cam) + "/camera/name", cams.front().name}});
        s = newScene();
        layer(s, cam);
        push();
        const auto st = waitState(cam, "ok", 8s);
        check("camera opens (" + cams.front().name + ")", st == "ok", st);
        shot("23-camera", 300ms);
    }

    // video file
    if (media_.empty()) {
        note("video file", "none given (--media FILE)");
    } else {
        const auto med = call("/video/source/add", {{"kind", std::string("media")}});
        set({{"/video/source/" + std::to_string(med) + "/file", media_}});
        s = newScene();
        layer(s, med);
        push();
        const auto st = waitState(med, "ok", 8s);
        check("video file plays (" + media_ + ")", st == "ok", st);
        auto a = shot("24-media-a", 300ms);
        auto b = shot("25-media-b", 1000ms);
        check("video file picture moves", a.bgra != b.bgra);
    }
}

void Test::fourMixes() {
    std::cout << "\n[four mixes]" << std::endl;
    const auto disp = call("/video/source/add", {{"kind", std::string("display")}});
    const auto txt = call("/video/source/add", {{"kind", std::string("text")}});
    set({{"/video/source/" + std::to_string(txt) + "/text/content", std::string("MIX")}});
    const auto s = call("/video/scene/add");
    layer(s, disp);
    const auto lt = layer(s, txt);
    set({{base(s, lt) + "/width", 0.3}, {base(s, lt) + "/height", 0.2}});
    std::vector<std::shared_ptr<VideoMix>> leases;
    std::vector<std::unique_ptr<Grabber>> grabbers;
    for (int i = 2; i <= 4; ++i) call("/video/mix/add");
    for (int i = 1; i <= 4; ++i)
        set({{"/video/mix/" + std::to_string(i) + "/program_scene", s},
             {"/video/mix/" + std::to_string(i) + "/transition/type", std::string("cut")}});
    set({{"/video/mix/4/width", std::int64_t{3840}}, {"/video/mix/4/height", std::int64_t{2160}}});
    push();
    for (int i = 2; i <= 4; ++i) {
        leases.push_back(engine_->acquire(i));
        grabbers.push_back(std::make_unique<Grabber>(*leases.back()));
    }
    std::this_thread::sleep_for(1500ms);
    const auto f0 = grabber_->frames();
    std::vector<std::uint64_t> g0;
    for (auto& g : grabbers) g0.push_back(g->frames());
    std::this_thread::sleep_for(5000ms);
    auto line = [&](int n, VideoMix& m, std::uint64_t frames) {
        const auto st = m.stats();
        char b[200];
        std::snprintf(b, sizeof b, "%llu frames in 5 s, tick %.2f / %.2f ms, late %llu",
                      static_cast<unsigned long long>(frames), st.tickMsAvg, st.tickMsMax,
                      static_cast<unsigned long long>(st.lateTicks));
        check("mix " + std::to_string(n) + " (" + std::to_string(m.settings().width) + "x" +
                  std::to_string(m.settings().height) + "@60) keeps its rate",
              frames >= 285, b);
    };
    line(1, *mix_, grabber_->frames() - f0);
    for (std::size_t i = 0; i < grabbers.size(); ++i) line(int(i + 2), *leases[i], grabbers[i]->frames() - g0[i]);
    savePng(grabbers.back()->grab(), out_ / "30-mix4-4k.png");
    grabbers.clear();
    leases.clear();
}

void Test::resize() {
    std::cout << "\n[canvas size change]" << std::endl;
    const auto g0 = grabber_->generation();
    set({{"/video/mix/1/width", std::int64_t{1280}}, {"/video/mix/1/height", std::int64_t{720}}});
    auto p = shot("31-resized", 300ms);
    check("canvas follows the new size", p.width == 1280 && p.height == 720,
          std::to_string(p.width) + "x" + std::to_string(p.height));
    check("generation went up", grabber_->generation() > g0);
}

int Test::run() {
    fs::create_directories(out_);
    for (const auto& c : proto::globalDefaults()) store_.publish({c});
    set({{"/video/mix/1/fps", std::int64_t{60}}});
    engine_ = std::make_unique<VideoEngine>();
    engine_->setReporter([this](const std::string& method, const proto::Args& args) {
        if (method != "/video/source/report") return;
        const auto id = std::get<std::int64_t>(args.at("source"));
        std::string err;
        if (auto it = args.find("error"); it != args.end()) err = std::get<std::string>(it->second);
        std::lock_guard lock(statusMutex_);
        status_[int(id)] = {std::get<std::string>(args.at("state")), err};
    });
    push();
    mix_ = engine_->acquire(1);
    if (!mix_) throw std::runtime_error("mix 1 missing");
    grabber_ = std::make_unique<Grabber>(*mix_);
    std::cout << "video mixer on " << engine_->adapterName() << ", pictures in " << out_.string() << std::endl;

    for (auto part : {&Test::basics, &Test::transitions, &Test::captures, &Test::fourMixes, &Test::resize}) {
        try {
            (this->*part)();
        } catch (const std::exception& e) {
            check("exception", false, e.what());
        }
    }
    if (!mix_->error().empty()) note("mixer error", mix_->error());

    std::FILE* f = _wfopen((out_ / "summary.md").wstring().c_str(), L"w");
    if (f) {
        std::fprintf(f, "# capd mixtest (%s)\n\n| check | result | detail |\n|---|---|---|\n", engine_->adapterName().c_str());
        for (const auto& r : results_) std::fprintf(f, "| %s | %s | %s |\n", r.name.c_str(), r.result.c_str(), r.detail.c_str());
        std::fclose(f);
    }
    grabber_.reset();
    mix_.reset();
    engine_.reset();
    std::cout << "\nRESULT " << (failures_ ? "FAIL" : "PASS") << " (" << failures_ << " failed of " << results_.size()
              << " checks)" << std::endl;
    return failures_ ? 1 : 0;
}

}  // namespace

int cmdMixTest(const std::vector<std::string>& args) {
    fs::path out = fs::current_path() / "mixtest";
    std::string media;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--out" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--media" && i + 1 < args.size()) media = args[++i];
        else throw std::runtime_error("mixtest options: --out DIR, --media FILE");
    }
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    int rc = Test(out, media).run();
    if (mf) MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    return rc;
}

}  // namespace dc::capd
