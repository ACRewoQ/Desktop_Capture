// DXGI Desktop Duplication source. Each new desktop image is copied (GPU to GPU) into our own texture, because the
// duplication surface is only valid until ReleaseFrame().

#include "capture_source.hpp"

#include <dxgi1_2.h>

namespace dc::capd {

namespace {

class DdaSource final : public CaptureSource {
public:
    DdaSource(const Device& dev, const MonitorInfo& monitor) : dev_(dev), output_(monitor.output) { open(); }

    ~DdaSource() override {
        if (dup_ && holding_) dup_->ReleaseFrame();
    }

    const char* name() const override { return "dda"; }

    bool poll() override {
        if (!dup_) {
            // Lost earlier (mode change, secure desktop, full-screen exclusive app). Retry now and then.
            if (GetTickCount64() < retryAt_) return false;
            try {
                open();
            } catch (const HrError&) {
                retryAt_ = GetTickCount64() + 250;
                return false;
            }
        }

        bool changed = false;
        // Drain: AcquireNextFrame accumulates updates, so one successful acquire gives the newest image.
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT hr = dup_->AcquireNextFrame(0, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            dup_.Reset();
            holding_ = false;
            ++recoveries_;
            return false;
        }
        DC_HR(hr);
        holding_ = true;

        // LastPresentTime == 0 means only the mouse moved; the image is unchanged.
        if (info.LastPresentTime.QuadPart != 0) {
            ComPtr<ID3D11Texture2D> src;
            DC_HR(resource.As(&src));
            D3D11_TEXTURE2D_DESC desc{};
            src->GetDesc(&desc);
            if (!texture_ || desc.Width != width_ || desc.Height != height_) {
                width_ = desc.Width;
                height_ = desc.Height;
                texture_ = createBgraTexture(dev_.device.Get(), width_, height_);
            }
            dev_.context->CopyResource(texture_.Get(), src.Get());
            frameQpc_ = info.LastPresentTime.QuadPart;
            changed = true;
        }
        dup_->ReleaseFrame();
        holding_ = false;
        return changed;
    }

    ID3D11Texture2D* texture() const override { return texture_.Get(); }
    UINT width() const override { return width_; }
    UINT height() const override { return height_; }
    std::int64_t frameQpc() const override { return frameQpc_; }
    std::uint64_t recoveries() const override { return recoveries_; }

private:
    void open() {
        ComPtr<IDXGIOutput1> out1;
        DC_HR(output_.As(&out1));
        ComPtr<IDXGIOutputDuplication> dup;
        DC_HR(out1->DuplicateOutput(dev_.device.Get(), &dup));
        // The image comes in the output's native orientation; rotated monitors are not handled yet (P3).
        dup_ = dup;
    }

    Device dev_;
    ComPtr<IDXGIOutput> output_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> texture_;
    UINT width_ = 0, height_ = 0;
    std::int64_t frameQpc_ = 0;
    std::uint64_t recoveries_ = 0;
    ULONGLONG retryAt_ = 0;
    bool holding_ = false;
};

}  // namespace

std::unique_ptr<CaptureSource> createDdaSource(const Device& dev, const MonitorInfo& monitor) {
    return std::make_unique<DdaSource>(dev, monitor);
}

}  // namespace dc::capd
