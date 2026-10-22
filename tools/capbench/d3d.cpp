#include "d3d.hpp"

#include <d3d10.h>

#include <cstdio>

namespace dc::capbench {

namespace {

std::string formatHr(const char* what, HRESULT hr) {
    char buf[64];
    std::snprintf(buf, sizeof buf, " failed (hr=0x%08lX)", static_cast<unsigned long>(hr));
    return std::string(what) + buf;
}

}  // namespace

HrError::HrError(const char* what, HRESULT hr) : std::runtime_error(formatHr(what, hr)), hr_(hr) {}

std::string narrow(const wchar_t* s) {
    if (!s || !*s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::vector<MonitorInfo> enumerateMonitors() {
    ComPtr<IDXGIFactory1> factory;
    DC_HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

    std::vector<MonitorInfo> list;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od{};
            output->GetDesc(&od);
            if (!od.AttachedToDesktop) continue;
            MonitorInfo m;
            m.index = static_cast<int>(list.size());
            m.adapterName = narrow(ad.Description);
            m.deviceName = narrow(od.DeviceName);
            m.monitor = od.Monitor;
            m.desktop = od.DesktopCoordinates;
            MONITORINFO mi{};
            mi.cbSize = sizeof mi;
            if (GetMonitorInfoW(od.Monitor, &mi)) m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            m.adapter = adapter;
            m.output = output;
            list.push_back(std::move(m));
        }
    }
    return list;
}

Device createDevice(IDXGIAdapter1* adapter) {
    Device d;
    d.adapter = adapter;
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    d.adapterName = narrow(ad.Description);

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    DC_HR(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, ARRAYSIZE(levels),
                            D3D11_SDK_VERSION, &d.device, nullptr, &d.context));

    // WGC's free-threaded frame pool and FFmpeg touch the device from other threads.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(d.device.As(&mt))) mt->SetMultithreadProtected(TRUE);
    return d;
}

ComPtr<ID3D11Texture2D> createBgraTexture(ID3D11Device* device, UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tex;
    DC_HR(device->CreateTexture2D(&desc, nullptr, &tex));
    return tex;
}

}  // namespace dc::capbench
