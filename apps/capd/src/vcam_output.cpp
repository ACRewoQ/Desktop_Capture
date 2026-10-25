#include "vcam_output.hpp"

#include "converter.hpp"

#include "dc/shmaudio/clock.hpp"
#include "dc/vcam/ids.hpp"
#include "dc/vcam/sender.hpp"

namespace dc::capd {

VcamOutput::VcamOutput(const std::string& resolution) {
    const auto kind = vcam::systemCameraKind();
    sender_ = std::make_unique<vcam::Sender>(kind);
    if (kind == vcam::CameraKind::DirectShow) {
        width_ = vcam::kDsWidth;
        height_ = vcam::kDsHeight;
        nv12_ = false;
    } else if (resolution == "720p") {
        width_ = 1280;
        height_ = 720;
    }
}

VcamOutput::~VcamOutput() { detach(); }

void VcamOutput::rebuild(const Device& dev) {
    dev_ = dev;
    for (auto& p : ring_) {
        p.staging.Reset();
        p.busy = false;
    }
    const DXGI_FORMAT format = nv12_ ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM;
    converter_ = std::make_unique<Converter>(dev, UINT(width_), UINT(height_), UINT(kFps), format);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(width_);
    desc.Height = UINT(height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    target_.Reset();
    DC_HR(dev.device->CreateTexture2D(&desc, nullptr, &target_));

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& p : ring_) DC_HR(dev.device->CreateTexture2D(&desc, nullptr, &p.staging));
}

// Hands every copy the GPU has finished to the camera, oldest first.
void VcamOutput::retire() {
    for (std::size_t k = 0; k < ring_.size(); ++k) {
        auto& p = ring_[(next_ + k) % ring_.size()];
        if (!p.busy) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = dev_.context->Map(p.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;  // later ones are not ready either
        DC_HR(hr);
        const auto* data = static_cast<const std::uint8_t*>(m.pData);
        if (nv12_) {
            // NV12 staging textures keep the UV plane right after height rows of the Y plane.
            sender_->sendNv12(data, int(m.RowPitch), data + std::size_t(m.RowPitch) * UINT(height_), int(m.RowPitch),
                              width_, height_, p.qpc);
        } else {
            sender_->sendBgra(data, int(m.RowPitch), width_, height_, p.qpc);
        }
        dev_.context->Unmap(p.staging.Get(), 0);
        p.busy = false;
        ++frames_;
    }
}

void VcamOutput::onFrame(ID3D11Texture2D* canvas, const Device& dev, std::uint64_t generation, std::int64_t qpc) {
    if (generation != generation_) {
        rebuild(dev);
        generation_ = generation;
        lastSlot_ = -1;
    }
    retire();

    // 30 fps whatever the mix rate: one frame per 1/30 s slot of the QPC clock.
    const std::int64_t slot = qpc * kFps / shm::qpcFrequency();
    if (slot != lastSlot_) {
        lastSlot_ = slot;
        auto& p = ring_[next_];
        if (p.busy) {
            ++skipped_;
        } else {
            converter_->convert(canvas, target_.Get(), 0);
            dev_.context->CopyResource(p.staging.Get(), target_.Get());
            p.busy = true;
            p.qpc = qpc;
            next_ = (next_ + 1) % ring_.size();
        }
    }

    if (qpc >= nextStatusQpc_) {
        nextStatusQpc_ = qpc + shm::qpcFrequency() / 2;
        const auto st = sender_->status();
        std::lock_guard lock(statusMutex_);
        cameraOpen_ = st.consumer;
        if (!st.error.empty()) state_ = st.error;
        else state_ = st.consumer ? "active" : "waiting";
    }
}

void VcamOutput::detach() {
    sender_->detach();
    std::lock_guard lock(statusMutex_);
    cameraOpen_ = false;
    state_ = "waiting";
}

VcamOutput::Stats VcamOutput::stats() const {
    Stats s;
    s.frames = frames_.load();
    s.skipped = skipped_.load();
    std::lock_guard lock(statusMutex_);
    s.cameraOpen = cameraOpen_;
    s.state = state_;
    return s;
}

}  // namespace dc::capd
