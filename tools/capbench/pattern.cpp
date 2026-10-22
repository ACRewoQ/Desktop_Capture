// Test pattern window: a borderless window that changes every vsync (moving bar + flickering tiles), so the capture
// source gets a new frame at the display rate and the encoder sees real motion. Rendered entirely on the GPU with
// ClearView, so it costs almost no CPU. Runs as its own process so its load is not counted in the bench.

#include "pattern.hpp"

#include "d3d.hpp"

#include <dxgi1_2.h>

#include <cstdint>
#include <cstdio>

namespace dc::capbench {

namespace {

LRESULT CALLBACK patternProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::uint32_t xorshift(std::uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

}  // namespace

int runPattern(const MonitorInfo& monitor, bool full, double seconds) {
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = patternProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DcCapbenchPattern";
    RegisterClassW(&wc);

    const RECT m = monitor.desktop;
    const int mw = m.right - m.left, mh = m.bottom - m.top;
    const int w = full ? mw : mw / 2, h = full ? mh : mh / 2;
    const int x = m.left + (mw - w) / 2, y = m.top + (mh - h) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"dc-capbench pattern",
                                WS_POPUP | WS_VISIBLE, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "pattern: CreateWindow failed\n");
        return 1;
    }

    Device dev = createDevice(monitor.adapter.Get());
    ComPtr<IDXGIDevice> dxgiDevice;
    DC_HR(dev.device.As(&dxgiDevice));
    ComPtr<IDXGIAdapter> adapter;
    DC_HR(dxgiDevice->GetAdapter(&adapter));
    ComPtr<IDXGIFactory2> factory;
    DC_HR(adapter->GetParent(IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = static_cast<UINT>(w);
    sd.Height = static_cast<UINT>(h);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap;
    DC_HR(factory->CreateSwapChainForHwnd(dev.device.Get(), hwnd, &sd, nullptr, nullptr, &swap));
    ComPtr<ID3D11Texture2D> back;
    DC_HR(swap->GetBuffer(0, IID_PPV_ARGS(&back)));
    ComPtr<ID3D11RenderTargetView> rtv;
    DC_HR(dev.device->CreateRenderTargetView(back.Get(), nullptr, &rtv));
    ComPtr<ID3D11DeviceContext1> ctx1;
    DC_HR(dev.context.As(&ctx1));

    constexpr int kCols = 16, kRows = 9;
    std::uint32_t tiles[kRows][kCols];
    std::uint32_t rng = 0x12345678u;
    for (auto& row : tiles)
        for (auto& t : row) t = xorshift(rng);
    const ULONGLONG start = GetTickCount64();
    for (std::uint64_t frame = 0;; ++frame) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (seconds > 0 && double(GetTickCount64() - start) / 1000.0 > seconds) break;

        const float bg[4] = {0.05f, 0.05f, 0.08f, 1.0f};
        ctx1->ClearView(rtv.Get(), bg, nullptr, 0);
        // A quarter of the tiles change colour every frame (keeps the encoder busy without being pure noise).
        for (int r = 0; r < kRows; ++r) {
            for (int c = 0; c < kCols; ++c) {
                if ((xorshift(rng) & 3) == 0) tiles[r][c] = xorshift(rng);
                const std::uint32_t v = tiles[r][c];
                const float col[4] = {float(v & 0xff) / 255.f, float((v >> 8) & 0xff) / 255.f,
                                      float((v >> 16) & 0xff) / 255.f, 1.0f};
                const D3D11_RECT rect{c * w / kCols + 2, r * h / kRows + 2, (c + 1) * w / kCols - 2,
                                      (r + 1) * h / kRows - 2};
                ctx1->ClearView(rtv.Get(), col, &rect, 1);
            }
        }
        // Bar sweeping across once per second at 60 Hz.
        const LONG barX = static_cast<LONG>((frame % 60) * static_cast<std::uint64_t>(w) / 60);
        const float white[4] = {1, 1, 1, 1};
        const D3D11_RECT bar{barX, 0, barX + w / 40, h};
        ctx1->ClearView(rtv.Get(), white, &bar, 1);

        swap->Present(1, 0);
    }
    DestroyWindow(hwnd);
    return 0;
}

}  // namespace dc::capbench
