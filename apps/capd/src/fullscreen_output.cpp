#include "fullscreen_output.hpp"

#include "converter.hpp"
#include "video_source.hpp"

#include <cstdlib>

namespace dc::capd {

namespace {

constexpr wchar_t kClassName[] = L"DesktopCaptureFullscreen";
constexpr UINT_PTR kCheckTimer = 1;

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);  // no mouse pointer over the picture
            return TRUE;
        }
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;  // the swap chain paints it
    case WM_CLOSE:
        return 0;  // only the output closes it (Alt+F4 must not remove a destination the engine owns)
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

std::optional<RECT> findMonitorRect(const std::string& id) {
    if (auto m = findMonitor(id)) return m->desktop;
    // An index from "capd list" (handy on the command line).
    if (!id.empty() && id.find_first_not_of("0123456789") == std::string::npos) {
        const auto displays = listDisplays();
        const auto index = static_cast<std::size_t>(std::atoi(id.c_str()));
        if (index < displays.size())
            if (auto m = findMonitor(displays[index].id)) return m->desktop;
    }
    return std::nullopt;
}

FullscreenOutput::FullscreenOutput(std::string monitorId, MonitorLookup lookup)
    : monitorId_(std::move(monitorId)), lookup_(lookup ? std::move(lookup) : MonitorLookup(findMonitorRect)) {
    thread_ = std::thread([this] { windowThread(); });
}

FullscreenOutput::~FullscreenOutput() {
    releaseSwapChain();
    quit_ = true;
    if (const DWORD id = threadId_.load()) PostThreadMessageW(id, WM_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
}

void FullscreenOutput::setState(std::string s) {
    std::lock_guard lock(stateMutex_);
    state_ = std::move(s);
}

void FullscreenOutput::place(const RECT& r) {
    const HWND hwnd = hwnd_.load();
    const int w = r.right - r.left, h = r.bottom - r.top;
    RECT now{};
    GetWindowRect(hwnd, &now);
    const bool moved = now.left != r.left || now.top != r.top || now.right != r.right || now.bottom != r.bottom;
    if (moved || !IsWindowVisible(hwnd)) {
        SetWindowPos(hwnd, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        width_ = w;
        height_ = h;
        ++placement_;
    }
}

void FullscreenOutput::windowThread() {
    threadId_ = GetCurrentThreadId();
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = inst;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // fails harmlessly when another output registered it already
    const HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClassName,
                                      L"Desktop Capture full screen", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, inst,
                                      nullptr);
    if (!hwnd) {
        setState("error: cannot create the window (" + std::to_string(GetLastError()) + ")");
        return;
    }
    hwnd_ = hwnd;

    auto check = [&] {
        if (const auto r = lookup_(monitorId_)) {
            place(*r);
            setState("active");
        } else {
            // The monitor is gone: only this output stops (the video thread drops its swap chain), and the
            // window comes back when the monitor does.
            ShowWindow(hwnd, SW_HIDE);
            width_ = 0;
            height_ = 0;
            ++placement_;
            setState("monitor not found: " + (monitorId_.empty() ? std::string("(primary)") : monitorId_));
        }
    };
    check();
    SetTimer(hwnd, kCheckTimer, 1000, nullptr);
    MSG msg;
    while (!quit_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if ((msg.hwnd == hwnd && msg.message == WM_TIMER && msg.wParam == kCheckTimer) || msg.message == WM_DISPLAYCHANGE)
            check();
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(hwnd, kCheckTimer);
    hwnd_ = nullptr;
    DestroyWindow(hwnd);
}

void FullscreenOutput::releaseSwapChain() {
    converter_.reset();
    if (waitable_) CloseHandle(waitable_);
    waitable_ = nullptr;
    swapChain_.Reset();
    bufferW_ = bufferH_ = 0;
}

bool FullscreenOutput::ensureSwapChain(const Device& dev, std::uint64_t generation) {
    const std::uint64_t placement = placement_.load();
    if (swapChain_ && generation == generation_ && placement == placementSeen_) return true;
    releaseSwapChain();
    generation_ = generation;
    placementSeen_ = placement;
    const HWND hwnd = hwnd_.load();
    const int w = width_.load(), h = height_.load();
    if (!hwnd || w <= 0 || h <= 0) return false;

    dev_ = dev;
    ComPtr<IDXGIFactory2> factory;
    DC_HR(dev.adapter->GetParent(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = UINT(w);
    desc.Height = UINT(h);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> sc1;
    DC_HR(factory->CreateSwapChainForHwnd(dev.device.Get(), hwnd, &desc, nullptr, nullptr, &sc1));
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    DC_HR(sc1.As(&swapChain_));
    DC_HR(swapChain_->SetMaximumFrameLatency(1));
    waitable_ = swapChain_->GetFrameLatencyWaitableObject();
    converter_ = std::make_unique<Converter>(dev, UINT(w), UINT(h), 60, DXGI_FORMAT_B8G8R8A8_UNORM);
    bufferW_ = UINT(w);
    bufferH_ = UINT(h);
    return true;
}

void FullscreenOutput::onFrame(ID3D11Texture2D* canvas, const Device& dev, std::uint64_t generation, std::int64_t) {
    try {
        if (!ensureSwapChain(dev, generation)) return;
        // Waitable swap chain: signalled when the display can take another frame. Never wait on the video thread.
        if (WaitForSingleObject(waitable_, 0) != WAIT_OBJECT_0) {
            ++skipped_;
            return;
        }
        ComPtr<ID3D11Texture2D> back;
        DC_HR(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back)));
        converter_->convert(canvas, back.Get(), 0);
        const HRESULT hr = swapChain_->Present(1, 0);
        if (FAILED(hr)) throw HrError("IDXGISwapChain::Present", hr);
        ++frames_;
    } catch (const std::exception& e) {
        // A lost device comes back with a new generation (the video engine recreates it); anything else is
        // retried on the next placement change. The other outputs of the mix are not affected.
        releaseSwapChain();
        setState(std::string("error: ") + e.what());
    }
}

FullscreenOutput::Stats FullscreenOutput::stats() const {
    Stats s;
    s.frames = frames_.load();
    s.skipped = skipped_.load();
    std::lock_guard lock(stateMutex_);
    s.state = state_;
    return s;
}

}  // namespace dc::capd
