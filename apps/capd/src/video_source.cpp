// Video mixer sources that need no Media Foundation: monitor and window capture (WGC / Desktop Duplication),
// image files (WIC), text (DirectWrite), single colour and the placeholder for LAN streams. Plus the monitor and
// window lists. Cameras and video files are in source_mf.cpp.

#include "video_source.hpp"

#include "capture_source.hpp"
#include "text_renderer.hpp"

#include <dwmapi.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <thread>

namespace dc::capd {

std::unique_ptr<VideoSource> createCameraSource(const SourceContext& ctx, const SourceConfig& cfg);  // source_mf.cpp
std::unique_ptr<VideoSource> createMediaSource(const SourceContext& ctx, const SourceConfig& cfg);   // source_mf.cpp

// --- TextureSlot ---

ID3D11Texture2D* TextureSlot::ensure(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, UINT bind) {
    if (texture_ && width == width_ && height == height_ && format == format_) return texture_.Get();
    reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    DC_HR(device->CreateTexture2D(&desc, nullptr, &texture_));
    width_ = width;
    height_ = height;
    format_ = format;
    return texture_.Get();
}

ID3D11ShaderResourceView* TextureSlot::view(ID3D11Device* device) {
    if (!texture_) return nullptr;
    if (!view_) DC_HR(device->CreateShaderResourceView(texture_.Get(), nullptr, &view_));
    return view_.Get();
}

void TextureSlot::reset() {
    view_.Reset();
    texture_.Reset();
    width_ = height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
}

namespace {

std::uint64_t nowMs() { return GetTickCount64(); }

// Shader view for a texture somebody else owns (a capture source's texture); recreated when it changes.
class ViewCache {
public:
    ID3D11ShaderResourceView* get(ID3D11Device* device, ID3D11Texture2D* texture) {
        if (!texture) return nullptr;
        if (texture != texture_.Get()) {
            view_.Reset();
            texture_ = texture;
            DC_HR(device->CreateShaderResourceView(texture, nullptr, &view_));
        }
        return view_.Get();
    }
    void reset() {
        view_.Reset();
        texture_.Reset();
    }

private:
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> view_;
};

std::string errorText(const std::exception& e) { return e.what(); }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

LUID adapterLuid(IDXGIAdapter1* adapter) {
    DXGI_ADAPTER_DESC1 d{};
    adapter->GetDesc1(&d);
    return d.AdapterLuid;
}

bool sameLuid(LUID a, LUID b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

// Monitor or window capture with retry: looks for the target every 2 s while it is missing and drops the
// capture when the OS closes it (monitor detached, window closed).
class CaptureVideoSource : public VideoSource {
public:
    CaptureVideoSource(const SourceContext& ctx, const SourceConfig& cfg) : dev_(*ctx.device), cfg_(cfg) {}

    bool poll() override {
        if (capture_ && capture_->closed()) {
            recoveries_ += capture_->recoveries();
            capture_.reset();
            views_.reset();
            status_ = {"disconnected", "", 0, 0};
            retryAt_ = 0;
        }
        if (!capture_) {
            if (nowMs() < retryAt_) return false;
            retryAt_ = nowMs() + 2000;
            try {
                capture_ = open();
            } catch (const winrt::hresult_error& e) {
                status_ = {"error", winrt::to_string(e.message()), 0, 0};
            } catch (const HrError& e) {
                if (deviceLost(e.code())) throw;
                status_ = {"error", errorText(e), 0, 0};
            } catch (const std::exception& e) {
                status_ = {"error", errorText(e), 0, 0};
            }
            if (!capture_) return false;
        }
        bool changed = false;
        try {
            changed = capture_->poll();
        } catch (const HrError& e) {
            if (deviceLost(e.code())) throw;
            status_ = {"error", errorText(e), 0, 0};
            capture_.reset();
            views_.reset();
            return false;
        } catch (const winrt::hresult_error& e) {
            status_ = {"error", winrt::to_string(e.message()), 0, 0};
            capture_.reset();
            views_.reset();
            return false;
        }
        if (capture_->texture()) status_ = {"ok", "", int(capture_->width()), int(capture_->height())};
        return changed;
    }

    ID3D11ShaderResourceView* view() override {
        return capture_ ? views_.get(dev_.device.Get(), capture_->texture()) : nullptr;
    }
    int width() const override { return capture_ ? int(capture_->width()) : 0; }
    int height() const override { return capture_ ? int(capture_->height()) : 0; }
    SourceStatus status() const override { return status_; }
    std::uint64_t recoveries() const override { return recoveries_ + (capture_ ? capture_->recoveries() : 0); }
    std::int64_t frameQpc() const override { return capture_ ? capture_->frameQpc() : 0; }

protected:
    static bool deviceLost(HRESULT hr) {
        return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
               hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }
    // Returns the capture, or null after setting status_ (target missing).
    virtual std::unique_ptr<CaptureSource> open() = 0;

    Device dev_;
    SourceConfig cfg_;
    SourceStatus status_;
    std::unique_ptr<CaptureSource> capture_;
    ViewCache views_;
    std::uint64_t retryAt_ = 0;
    std::uint64_t recoveries_ = 0;
};

class DisplaySource final : public CaptureVideoSource {
public:
    using CaptureVideoSource::CaptureVideoSource;

    std::string description() const override {
        return cfg_.displayMethod + " " + (gdiName_.empty() ? "(no monitor)" : gdiName_) + " " + std::to_string(width()) +
               "x" + std::to_string(height());
    }

protected:
    std::unique_ptr<CaptureSource> open() override {
        const auto m = findMonitor(cfg_.displayId);
        if (!m) {
            status_ = {"disconnected", "", 0, 0};
            return nullptr;
        }
        gdiName_ = m->deviceName;
        if (cfg_.displayMethod == "dda") {
            // Desktop Duplication only works on the GPU that drives the monitor.
            if (!sameLuid(adapterLuid(m->adapter.Get()), adapterLuid(dev_.adapter.Get())))
                throw std::runtime_error("the monitor is on another GPU (" + m->adapterName + "); use wgc");
            return createDdaSource(dev_, *m);
        }
        return createWgcSource(dev_, *m, cfg_.cursor);
    }

private:
    std::string gdiName_;
};

class WindowSource final : public CaptureVideoSource {
public:
    using CaptureVideoSource::CaptureVideoSource;

    std::string description() const override {
        return "window " + cfg_.windowExe + " \"" + cfg_.windowTitle + "\" " + std::to_string(width()) + "x" +
               std::to_string(height());
    }

protected:
    std::unique_ptr<CaptureSource> open() override {
        if (cfg_.windowExe.empty() && cfg_.windowTitle.empty()) {
            status_ = {"disconnected", "", 0, 0};
            return nullptr;
        }
        const auto exe = lower(cfg_.windowExe);
        const auto windows = listWindows();
        const WindowInfo* target = nullptr;
        for (const auto& w : windows) {
            const bool exeOk = exe.empty() || lower(w.exe) == exe;
            const bool titleOk = cfg_.windowMatch == "exe" || w.title == cfg_.windowTitle;
            if (exeOk && titleOk) {
                target = &w;
                break;
            }
        }
        if (!target) {
            status_ = {"disconnected", "", 0, 0};
            return nullptr;
        }
        return createWgcWindowSource(dev_, target->hwnd, cfg_.cursor);
    }
};

// Image file, decoded by WIC on a worker thread and uploaded on the video thread. A file that cannot be read
// is tried again every 5 s (it may appear later, e.g. on a network drive).
class ImageSource final : public VideoSource {
public:
    ImageSource(const SourceContext& ctx, const SourceConfig& cfg) : dev_(*ctx.device), cfg_(cfg) { startLoad(); }
    ~ImageSource() override {
        if (worker_.joinable()) worker_.join();
    }

    bool poll() override {
        std::lock_guard lock(mutex_);
        if (loaded_) {
            loaded_ = false;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w_;
            desc.Height = h_;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{pixels_.data(), w_ * 4, 0};
            texture_.Reset();
            view_.Reset();
            DC_HR(dev_.device->CreateTexture2D(&desc, &init, &texture_));
            DC_HR(dev_.device->CreateShaderResourceView(texture_.Get(), nullptr, &view_));
            pixels_.clear();
            pixels_.shrink_to_fit();
            status_ = {"ok", "", int(w_), int(h_)};
            return true;
        }
        if (failed_ && !busy_ && nowMs() >= retryAt_) {
            failed_ = false;
            if (worker_.joinable()) worker_.join();
            startLoadLocked();
        }
        return false;
    }

    ID3D11ShaderResourceView* view() override { return view_.Get(); }
    int width() const override { return texture_ ? int(w_) : 0; }
    int height() const override { return texture_ ? int(h_) : 0; }
    AlphaMode alpha() const override { return AlphaMode::Straight; }
    SourceStatus status() const override {
        std::lock_guard lock(mutex_);
        return status_;
    }
    std::string description() const override { return "image " + cfg_.file; }

private:
    void startLoad() {
        std::lock_guard lock(mutex_);
        startLoadLocked();
    }
    void startLoadLocked() {
        busy_ = true;
        worker_ = std::thread([this] { load(); });
    }

    void load() {
        std::vector<std::uint8_t> pixels;
        UINT w = 0, h = 0;
        std::string err;
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            if (cfg_.file.empty()) throw std::runtime_error("no file chosen");
            ComPtr<IWICImagingFactory> wic;
            DC_HR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
            ComPtr<IWICBitmapDecoder> decoder;
            const HRESULT hr = wic->CreateDecoderFromFilename(widen(cfg_.file).c_str(), nullptr, GENERIC_READ,
                                                              WICDecodeMetadataCacheOnDemand, &decoder);
            if (FAILED(hr)) throw std::runtime_error("cannot open " + cfg_.file);
            ComPtr<IWICBitmapFrameDecode> frame;
            DC_HR(decoder->GetFrame(0, &frame));
            ComPtr<IWICFormatConverter> conv;
            DC_HR(wic->CreateFormatConverter(&conv));
            DC_HR(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom));
            DC_HR(conv->GetSize(&w, &h));
            if (w == 0 || h == 0 || w > 16384 || h > 16384) throw std::runtime_error("image size not supported");
            pixels.resize(size_t(w) * h * 4);
            DC_HR(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data()));
        } catch (const std::exception& e) {
            err = e.what();
        }
        if (SUCCEEDED(co)) CoUninitialize();
        std::lock_guard lock(mutex_);
        busy_ = false;
        if (err.empty()) {
            pixels_ = std::move(pixels);
            w_ = w;
            h_ = h;
            loaded_ = true;
        } else {
            failed_ = true;
            retryAt_ = nowMs() + 5000;
            status_ = {"error", err, 0, 0};
        }
    }

    Device dev_;
    SourceConfig cfg_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::vector<std::uint8_t> pixels_;
    UINT w_ = 0, h_ = 0;
    bool loaded_ = false, failed_ = false, busy_ = false;
    std::uint64_t retryAt_ = 0;
    SourceStatus status_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> view_;
};

class TextSource final : public VideoSource {
public:
    TextSource(const SourceContext& ctx, const SourceConfig& cfg) : dev_(*ctx.device), text_(ctx.text), cfg_(cfg) {}

    bool poll() override {
        if (drawn_) return false;
        drawn_ = true;
        UINT w = 0, h = 0;
        try {
            texture_ = text_->render(cfg_.text, &w, &h);
        } catch (const HrError& e) {
            if (e.code() == DXGI_ERROR_DEVICE_REMOVED || e.code() == DXGI_ERROR_DEVICE_RESET) throw;
            status_ = {"error", e.what(), 0, 0};
            return false;
        }
        view_.Reset();
        if (texture_) DC_HR(dev_.device->CreateShaderResourceView(texture_.Get(), nullptr, &view_));
        w_ = int(w);
        h_ = int(h);
        status_ = {"ok", "", w_, h_};
        return true;
    }

    ID3D11ShaderResourceView* view() override { return view_.Get(); }
    int width() const override { return w_; }
    int height() const override { return h_; }
    AlphaMode alpha() const override { return AlphaMode::Premultiplied; }
    SourceStatus status() const override { return status_; }
    std::string description() const override { return "text"; }

private:
    Device dev_;
    TextRenderer* text_;
    SourceConfig cfg_;
    bool drawn_ = false;
    int w_ = 0, h_ = 0;
    SourceStatus status_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> view_;
};

// Single colour (drawn by the compositor) and kinds without an implementation yet: only a status.
class StatusOnlySource final : public VideoSource {
public:
    StatusOnlySource(SourceStatus s, std::string description) : status_(std::move(s)), description_(std::move(description)) {}
    bool poll() override { return false; }
    ID3D11ShaderResourceView* view() override { return nullptr; }
    int width() const override { return 0; }
    int height() const override { return 0; }
    SourceStatus status() const override { return status_; }
    std::string description() const override { return description_; }

private:
    SourceStatus status_;
    std::string description_;
};

std::string exeOfWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(path));
    std::string exe;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        const std::wstring p(path, size);
        const auto slash = p.find_last_of(L"\\/");
        exe = narrow((slash == std::wstring::npos ? p : p.substr(slash + 1)).c_str());
    }
    CloseHandle(process);
    return exe;
}

BOOL CALLBACK collectWindow(HWND hwnd, LPARAM param) {
    auto* out = reinterpret_cast<std::vector<WindowInfo>*>(param);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return TRUE;
    wchar_t title[512];
    const int n = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    if (n <= 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    WindowInfo w;
    w.hwnd = hwnd;
    w.title = narrow(std::wstring(title, static_cast<size_t>(n)).c_str());
    w.exe = exeOfWindow(hwnd);
    out->push_back(std::move(w));
    return TRUE;
}

}  // namespace

std::vector<DisplayInfo> listDisplays() {
    // Stable IDs and friendly names come from the display configuration; size and primary from DXGI / GDI.
    struct Named {
        std::string gdi, path, name;
    };
    std::vector<Named> named;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) ==
            ERROR_SUCCESS) {
            paths.resize(pathCount);
            for (const auto& p : paths) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
                src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                src.header.size = sizeof src;
                src.header.adapterId = p.sourceInfo.adapterId;
                src.header.id = p.sourceInfo.id;
                DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
                tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tgt.header.size = sizeof tgt;
                tgt.header.adapterId = p.targetInfo.adapterId;
                tgt.header.id = p.targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
                const bool haveTarget = DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS;
                Named n;
                n.gdi = narrow(src.viewGdiDeviceName);
                if (haveTarget) {
                    n.path = narrow(tgt.monitorDevicePath);
                    n.name = narrow(tgt.monitorFriendlyDeviceName);
                }
                const bool dup = std::any_of(named.begin(), named.end(), [&](const Named& x) { return x.gdi == n.gdi; });
                if (!dup) named.push_back(std::move(n));  // mirrored monitors share one source
            }
        }
    }
    std::vector<DisplayInfo> out;
    for (const auto& m : enumerateMonitors()) {
        DisplayInfo d;
        d.gdiName = m.deviceName;
        d.id = m.deviceName;
        d.name = m.deviceName;
        for (const auto& n : named) {
            if (n.gdi != m.deviceName) continue;
            if (!n.path.empty()) d.id = n.path;
            if (!n.name.empty()) d.name = n.name;
        }
        d.width = int(m.desktop.right - m.desktop.left);
        d.height = int(m.desktop.bottom - m.desktop.top);
        d.primary = m.primary;
        out.push_back(std::move(d));
    }
    return out;
}

std::unique_ptr<MonitorInfo> findMonitor(const std::string& id) {
    const auto displays = listDisplays();
    const DisplayInfo* target = nullptr;
    for (const auto& d : displays) {
        if (id.empty() ? d.primary : (d.id == id || d.gdiName == id)) {
            target = &d;
            break;
        }
    }
    if (!target && id.empty() && !displays.empty()) target = &displays.front();
    if (!target) return nullptr;
    for (auto& m : enumerateMonitors())
        if (m.deviceName == target->gdiName) return std::make_unique<MonitorInfo>(std::move(m));
    return nullptr;
}

std::vector<WindowInfo> listWindows() {
    std::vector<WindowInfo> out;
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&out));
    return out;
}

std::unique_ptr<VideoSource> createVideoSource(const SourceContext& ctx, const SourceConfig& cfg) {
    if (cfg.kind == "display") return std::make_unique<DisplaySource>(ctx, cfg);
    if (cfg.kind == "window") return std::make_unique<WindowSource>(ctx, cfg);
    if (cfg.kind == "camera") return createCameraSource(ctx, cfg);
    if (cfg.kind == "media") return createMediaSource(ctx, cfg);
    if (cfg.kind == "image") return std::make_unique<ImageSource>(ctx, cfg);
    if (cfg.kind == "text") return std::make_unique<TextSource>(ctx, cfg);
    if (cfg.kind == "color") return std::make_unique<StatusOnlySource>(SourceStatus{"ok", "", 0, 0}, "color");
    if (cfg.kind == "lan")
        return std::make_unique<StatusOnlySource>(SourceStatus{"error", "LAN video streams are not available yet", 0, 0}, "lan");
    return std::make_unique<StatusOnlySource>(SourceStatus{"error", "unknown source kind " + cfg.kind, 0, 0}, cfg.kind);
}

}  // namespace dc::capd
