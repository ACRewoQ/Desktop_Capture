// Direct3D 11 helpers shared by the capture sources, the converter and the encoder.
#pragma once

#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace dc::capd {

using Microsoft::WRL::ComPtr;

// Thrown for any failed HRESULT. what() includes the call site text and the code.
class HrError : public std::runtime_error {
public:
    HrError(const char* what, HRESULT hr);
    HRESULT code() const { return hr_; }

private:
    HRESULT hr_;
};

#define DC_HR(expr)                                                                                                    \
    do {                                                                                                               \
        const HRESULT dc_hr_ = (expr);                                                                                 \
        if (FAILED(dc_hr_)) throw ::dc::capd::HrError(#expr, dc_hr_);                                              \
    } while (0)

std::string narrow(const wchar_t* s);

struct MonitorInfo {
    int index = 0;                 // position in the list (what --monitor takes)
    std::string adapterName;
    std::string deviceName;        // \\.\DISPLAY1
    HMONITOR monitor = nullptr;
    RECT desktop{};                // desktop coordinates (physical pixels)
    bool primary = false;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
};

std::vector<MonitorInfo> enumerateMonitors();

// D3D11 device with video support (for ID3D11VideoProcessor) and multithread protection on, created on the adapter
// that drives the captured monitor (WGC and Desktop Duplication both need that on hybrid-GPU machines).
struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIAdapter1> adapter;
    std::string adapterName;
};

Device createDevice(IDXGIAdapter1* adapter);

// Small BGRA texture helper used to keep the newest captured image.
ComPtr<ID3D11Texture2D> createBgraTexture(ID3D11Device* device, UINT width, UINT height);

}  // namespace dc::capd
