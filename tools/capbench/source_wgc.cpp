// Windows.Graphics.Capture source (monitor capture). Uses a free-threaded frame pool that the bench loop drains with
// TryGetNextFrame(), so no dispatcher queue or message loop is needed. The newest surface is copied (GPU to GPU) into
// our own texture so the pool buffer can be returned at once.

#include "capture_source.hpp"

#include <inspectable.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace dc::capbench {

namespace {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

constexpr int kPoolBuffers = 2;

class WgcSource final : public CaptureSource {
public:
    WgcSource(const Device& dev, const MonitorInfo& monitor) : dev_(dev) {
        if (!wgc::GraphicsCaptureSession::IsSupported())
            throw std::runtime_error("Windows.Graphics.Capture is not supported on this system");

        ComPtr<IDXGIDevice> dxgi;
        DC_HR(dev_.device.As(&dxgi));
        winrt::com_ptr<::IInspectable> inspectable;
        DC_HR(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
        rtDevice_ = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        DC_HR(interop->CreateForMonitor(monitor.monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                        winrt::put_abi(item_)));

        size_ = item_.Size();
        pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            rtDevice_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, kPoolBuffers, size_);
        session_ = pool_.CreateCaptureSession(item_);
        session_.IsCursorCaptureEnabled(true);
        // Win11: no yellow border. Older builds lack the property; the border is harmless for the bench.
        try {
            if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                    L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired"))
                session_.IsBorderRequired(false);
        } catch (const winrt::hresult_error&) {
        }
        session_.StartCapture();

        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq_ = f.QuadPart;
    }

    ~WgcSource() override {
        try {
            session_.Close();
            pool_.Close();
        } catch (const winrt::hresult_error&) {
        }
    }

    const char* name() const override { return "wgc"; }

    bool poll() override {
        // Keep only the newest frame; older ones are dropped (the pool holds at most kPoolBuffers).
        wgc::Direct3D11CaptureFrame newest{nullptr};
        for (;;) {
            auto f = pool_.TryGetNextFrame();
            if (!f) break;
            if (newest) newest.Close();
            newest = f;
        }
        if (!newest) return false;

        const auto contentSize = newest.ContentSize();
        if (contentSize.Width != size_.Width || contentSize.Height != size_.Height) {
            // Monitor resolution changed: resize the pool and skip this frame.
            size_ = contentSize;
            newest.Close();
            pool_.Recreate(rtDevice_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, kPoolBuffers, size_);
            ++recoveries_;
            return false;
        }

        auto access = newest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> src;
        DC_HR(access->GetInterface(IID_PPV_ARGS(&src)));
        D3D11_TEXTURE2D_DESC desc{};
        src->GetDesc(&desc);
        const UINT w = static_cast<UINT>(size_.Width), h = static_cast<UINT>(size_.Height);
        if (!texture_ || w != width_ || h != height_) {
            width_ = w;
            height_ = h;
            texture_ = createBgraTexture(dev_.device.Get(), width_, height_);
        }
        if (desc.Width == w && desc.Height == h) {
            dev_.context->CopyResource(texture_.Get(), src.Get());
        } else {
            // Pool surfaces can be larger than the content.
            const D3D11_BOX box{0, 0, 0, w, h, 1};
            dev_.context->CopySubresourceRegion(texture_.Get(), 0, 0, 0, 0, src.Get(), 0, &box);
        }
        // SystemRelativeTime is QPC-based, in 100 ns units.
        const auto t100ns = newest.SystemRelativeTime().count();
        frameQpc_ = static_cast<std::int64_t>(static_cast<double>(t100ns) * static_cast<double>(qpcFreq_) / 1e7);
        newest.Close();
        return true;
    }

    ID3D11Texture2D* texture() const override { return texture_.Get(); }
    UINT width() const override { return width_; }
    UINT height() const override { return height_; }
    std::int64_t frameQpc() const override { return frameQpc_; }
    std::uint64_t recoveries() const override { return recoveries_; }

private:
    Device dev_;
    wgd::Direct3D11::IDirect3DDevice rtDevice_{nullptr};
    wgc::GraphicsCaptureItem item_{nullptr};
    wgc::Direct3D11CaptureFramePool pool_{nullptr};
    wgc::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::SizeInt32 size_{};
    ComPtr<ID3D11Texture2D> texture_;
    UINT width_ = 0, height_ = 0;
    std::int64_t frameQpc_ = 0;
    std::int64_t qpcFreq_ = 1;
    std::uint64_t recoveries_ = 0;
};

}  // namespace

std::unique_ptr<CaptureSource> createWgcSource(const Device& dev, const MonitorInfo& monitor) {
    static bool apartment = false;
    if (!apartment) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;
    }
    return std::make_unique<WgcSource>(dev, monitor);
}

}  // namespace dc::capbench
