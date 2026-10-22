#include "d3d_video.hpp"

#include <dxgi1_2.h>
#include <mfobjects.h>

#include <cstring>
#include <iostream>

namespace v6 {

ComPtr<ID3D11Device> createVideoDevice(const LUID* luid, std::string* adapterName) {
    ComPtr<IDXGIFactory1> factory;
    V6_CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> chosen;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (!luid || (d.AdapterLuid.LowPart == luid->LowPart && d.AdapterLuid.HighPart == luid->HighPart)) {
            chosen = a;
            if (adapterName) *adapterName = toUtf8(d.Description);
            break;
        }
    }
    if (!chosen) throw std::runtime_error("display adapter not found");

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ComPtr<ID3D11Device> dev;
    V6_CHECK(D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &dev, nullptr, nullptr));
    ComPtr<ID3D10Multithread> mt;
    V6_CHECK(dev.As(&mt));
    mt->SetMultithreadProtected(TRUE);  // required when Media Foundation shares the device
    return dev;
}

LUID adapterLuidOf(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi;
    V6_CHECK(device->QueryInterface(IID_PPV_ARGS(&dxgi)));
    ComPtr<IDXGIAdapter> adapter;
    V6_CHECK(dxgi->GetAdapter(&adapter));
    DXGI_ADAPTER_DESC d;
    V6_CHECK(adapter->GetDesc(&d));
    return d.AdapterLuid;
}

// ---------------------------------------------------------------------------------------------------------------

FrameConverter::FrameConverter(ID3D11Device* device, int outWidth, int outHeight)
    : dev_(device), outW_(outWidth), outH_(outHeight) {
    dev_->GetImmediateContext(&ctx_);
    V6_CHECK(dev_.As(&vdev_));
    V6_CHECK(ctx_.As(&vctx_));
}

void FrameConverter::ensureProcessor(UINT inW, UINT inH) {
    if (vp_ && inW == vpInW_ && inH == vpInH_) return;
    inViews_.clear();
    outViews_.clear();
    vp_.Reset();
    vpEnum_.Reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {60, 1};
    cd.InputWidth = inW;
    cd.InputHeight = inH;
    cd.OutputFrameRate = {60, 1};
    cd.OutputWidth = static_cast<UINT>(outW_);
    cd.OutputHeight = static_cast<UINT>(outH_);
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    V6_CHECK(vdev_->CreateVideoProcessorEnumerator(&cd, &vpEnum_));
    UINT flags = 0;
    V6_CHECK(vpEnum_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags));
    if (!(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) throw std::runtime_error("VP: NV12 input unsupported");
    V6_CHECK(vpEnum_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &flags));
    if (!(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) throw std::runtime_error("VP: BGRA output unsupported");
    V6_CHECK(vdev_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_));
    vpInW_ = inW;
    vpInH_ = inH;

    // Fixed per-stream state.
    vctx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
    in.YCbCr_Matrix = 1;  // BT.709
    in.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
    out.RGB_Range = 0;  // full range
    vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out);
    RECT dst{0, 0, outW_, outH_};
    vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
    vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &dst);
}

ID3D11VideoProcessorInputView* FrameConverter::inputView(ID3D11Texture2D* tex, UINT slice) {
    auto key = std::make_pair(tex, slice);
    auto it = inViews_.find(key);
    if (it != inViews_.end()) return it->second.Get();
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
    d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    d.Texture2D.ArraySlice = slice;
    ComPtr<ID3D11VideoProcessorInputView> v;
    V6_CHECK(vdev_->CreateVideoProcessorInputView(tex, vpEnum_.Get(), &d, &v));
    return (inViews_[key] = v).Get();
}

ID3D11VideoProcessorOutputView* FrameConverter::outputView(ID3D11Texture2D* tex) {
    auto it = outViews_.find(tex);
    if (it != outViews_.end()) return it->second.Get();
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d{};
    d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> v;
    V6_CHECK(vdev_->CreateVideoProcessorOutputView(tex, vpEnum_.Get(), &d, &v));
    return (outViews_[tex] = v).Get();
}

ID3D11Texture2D* FrameConverter::uploadSoftwareFrame(IMFMediaBuffer* buf, const VideoInfo& info) {
    const UINT w = info.codedWidth, h = info.codedHeight;
    D3D11_TEXTURE2D_DESC have{};
    if (staging_) staging_->GetDesc(&have);
    if (!staging_ || have.Width != w || have.Height != h) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_NV12;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        staging_.Reset();
        uploaded_.Reset();
        V6_CHECK(dev_->CreateTexture2D(&d, nullptr, &staging_));
        // The video processor input needs a GPU texture; drivers differ in which bind flags they accept.
        d.Usage = D3D11_USAGE_DEFAULT;
        d.CPUAccessFlags = 0;
        for (UINT bind : {0u, static_cast<UINT>(D3D11_BIND_SHADER_RESOURCE), static_cast<UINT>(D3D11_BIND_DECODER),
                          static_cast<UINT>(D3D11_BIND_RENDER_TARGET)}) {
            d.BindFlags = bind;
            ComPtr<ID3D11Texture2D> t;
            if (FAILED(dev_->CreateTexture2D(&d, nullptr, &t))) continue;
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC vd{};
            vd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            ComPtr<ID3D11VideoProcessorInputView> v;
            if (FAILED(vdev_->CreateVideoProcessorInputView(t.Get(), vpEnum_.Get(), &vd, &v))) continue;
            uploaded_ = t;
            inViews_[std::make_pair(t.Get(), 0u)] = v;
            break;
        }
        if (!uploaded_) throw std::runtime_error("no NV12 texture usable as video processor input");
    }

    BYTE* src = nullptr;
    LONG pitch = 0;
    ComPtr<IMF2DBuffer> b2d;
    const bool is2d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2d))) && SUCCEEDED(b2d->Lock2D(&src, &pitch));
    if (!is2d) {
        V6_CHECK(buf->Lock(&src, nullptr, nullptr));
        pitch = info.stride < 0 ? -info.stride : info.stride;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx_->Map(staging_.Get(), 0, D3D11_MAP_WRITE, 0, &m);
    if (SUCCEEDED(hr)) {
        auto* dst = static_cast<BYTE*>(m.pData);
        for (UINT y = 0; y < h; ++y) std::memcpy(dst + y * m.RowPitch, src + static_cast<size_t>(y) * pitch, w);
        BYTE* dstUv = dst + static_cast<size_t>(m.RowPitch) * h;
        const BYTE* srcUv = src + static_cast<size_t>(pitch) * h;
        for (UINT y = 0; y < h / 2; ++y)
            std::memcpy(dstUv + y * m.RowPitch, srcUv + static_cast<size_t>(y) * pitch, w);
        ctx_->Unmap(staging_.Get(), 0);
    }
    if (is2d) b2d->Unlock2D();
    else buf->Unlock();
    V6_CHECK(hr);
    ctx_->CopyResource(uploaded_.Get(), staging_.Get());
    return uploaded_.Get();
}

void FrameConverter::convert(IMFSample* sample, const VideoInfo& info, ID3D11Texture2D* target) {
    ComPtr<IMFMediaBuffer> buf;
    V6_CHECK(sample->GetBufferByIndex(0, &buf));
    ComPtr<IMFDXGIBuffer> dx;
    ComPtr<ID3D11Texture2D> src;
    UINT slice = 0;
    if (SUCCEEDED(buf.As(&dx))) {
        V6_CHECK(dx->GetResource(IID_PPV_ARGS(&src)));
        V6_CHECK(dx->GetSubresourceIndex(&slice));
        D3D11_TEXTURE2D_DESC d;
        src->GetDesc(&d);
        ensureProcessor(d.Width, d.Height);
    } else {
        ensureProcessor(info.codedWidth, info.codedHeight);
        src = uploadSoftwareFrame(buf.Get(), info);
    }
    vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &info.visible);
    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = inputView(src.Get(), slice);
    V6_CHECK(vctx_->VideoProcessorBlt(vp_.Get(), outputView(target), 0, 1, &st));
}

// ---------------------------------------------------------------------------------------------------------------

SharedFrameRing::SharedFrameRing(ID3D11Device* device, int width, int height) : w_(width), h_(height) {
    device->GetImmediateContext(&ctx_);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(width);
    d.Height = static_cast<UINT>(height);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    for (int i = 0; i < kSlots; ++i) {
        V6_CHECK(device->CreateTexture2D(&d, nullptr, &tex_[i]));
        V6_CHECK(tex_[i].As(&mutex_[i]));
        ComPtr<IDXGIResource1> res;
        V6_CHECK(tex_[i].As(&res));
        V6_CHECK(res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                         &handles_[i]));
    }
}

SharedFrameRing::~SharedFrameRing() {
    for (HANDLE h : handles_)
        if (h) CloseHandle(h);
}

ID3D11Texture2D* SharedFrameRing::beginWrite() {
    // Key 0 = free (display has copied it), key 1 = holds a frame the display has not copied yet. Either is
    // fine to overwrite; start with the slot after the newest so the newest stays available to the display.
    for (int attempt = 0; attempt < kSlots; ++attempt) {
        const int s = (lastWritten_ + 1 + attempt) % kSlots;
        if (mutex_[s]->AcquireSync(0, 0) == S_OK || mutex_[s]->AcquireSync(1, 0) == S_OK) {
            writing_ = s;
            return tex_[s].Get();
        }
    }
    // The display is copying; that takes well under a millisecond.
    const int s = (lastWritten_ + 1) % kSlots;
    const HRESULT hr = mutex_[s]->AcquireSync(0, 100);
    if (hr != S_OK) throw HrError("shared texture stayed locked by the display side", hr);
    writing_ = s;
    return tex_[s].Get();
}

void SharedFrameRing::endWrite(std::uint64_t frameId) {
    const int s = writing_;
    ids_[s].store(frameId, std::memory_order_release);
    ctx_->Flush();
    V6_CHECK(mutex_[s]->ReleaseSync(1));
    lastWritten_ = s;
    writing_ = -1;
    latest_.store(s, std::memory_order_release);
    seq_.fetch_add(1, std::memory_order_acq_rel);
}

SharedFrameReader::SharedFrameReader(const SharedFrameRing& ring, ID3D11Device* displayDevice) : ring_(ring) {
    ComPtr<ID3D11Device1> dev1;
    V6_CHECK(displayDevice->QueryInterface(IID_PPV_ARGS(&dev1)));
    for (int i = 0; i < SharedFrameRing::kSlots; ++i) {
        V6_CHECK(dev1->OpenSharedResource1(ring.sharedHandle(i), IID_PPV_ARGS(&tex_[i])));
        V6_CHECK(tex_[i].As(&mutex_[i]));
    }
}

bool SharedFrameReader::fetch(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, std::uint64_t* frameId) {
    const std::uint64_t seq = ring_.sequence();
    if (seq == lastSeq_) return false;
    const int s = ring_.latestSlot();
    if (s < 0) return false;
    if (mutex_[s]->AcquireSync(1, 0) != S_OK) return false;  // being rewritten; a newer frame follows
    ctx->CopyResource(dst, tex_[s].Get());
    if (frameId) *frameId = ring_.frameIdOf(s);
    mutex_[s]->ReleaseSync(0);
    lastSeq_ = seq;
    return true;
}

}  // namespace v6
