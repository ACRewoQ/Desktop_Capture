#include "converter.hpp"

#include <algorithm>

namespace dc::capd {

Converter::Converter(const Device& dev, UINT outWidth, UINT outHeight, UINT fps)
    : dev_(dev), outWidth_(outWidth), outHeight_(outHeight), fps_(fps) {
    DC_HR(dev_.device.As(&videoDevice_));
    DC_HR(dev_.context.As(&videoContext_));
}

void Converter::setup(UINT inWidth, UINT inHeight) {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = inWidth;
    desc.InputHeight = inHeight;
    desc.InputFrameRate = {fps_, 1};
    desc.OutputWidth = outWidth_;
    desc.OutputHeight = outHeight_;
    desc.OutputFrameRate = {fps_, 1};
    desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    enumerator_.Reset();
    processor_.Reset();
    DC_HR(videoDevice_->CreateVideoProcessorEnumerator(&desc, &enumerator_));

    UINT support = 0;
    DC_HR(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support));
    if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
        throw std::runtime_error("the GPU's video processor cannot output NV12");

    DC_HR(videoDevice_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_));
    inWidth_ = inWidth;
    inHeight_ = inHeight;
    inputFor_ = nullptr;
    inputView_.Reset();
    outputViews_.clear();

    ID3D11VideoContext* vc = videoContext_.Get();
    ComPtr<ID3D11VideoContext1> vc1;
    if (SUCCEEDED(videoContext_.As(&vc1))) {
        vc1->VideoProcessorSetStreamColorSpace1(processor_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        vc1->VideoProcessorSetOutputColorSpace1(processor_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
        in.RGB_Range = 0;  // full range RGB
        in.YCbCr_Matrix = 1;
        vc->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &in);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
        out.YCbCr_Matrix = 1;  // BT.709
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vc->VideoProcessorSetOutputColorSpace(processor_.Get(), &out);
    }

    // Letterbox: keep the aspect ratio inside the output frame.
    const double scale = std::min(double(outWidth_) / inWidth, double(outHeight_) / inHeight);
    const LONG w = static_cast<LONG>(inWidth * scale + 0.5) & ~1L;
    const LONG h = static_cast<LONG>(inHeight * scale + 0.5) & ~1L;
    const LONG x = ((static_cast<LONG>(outWidth_) - w) / 2) & ~1L;
    const LONG y = ((static_cast<LONG>(outHeight_) - h) / 2) & ~1L;
    const RECT src{0, 0, static_cast<LONG>(inWidth), static_cast<LONG>(inHeight)};
    const RECT dst{x, y, x + w, y + h};
    const RECT target{0, 0, static_cast<LONG>(outWidth_), static_cast<LONG>(outHeight_)};
    vc->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vc->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &src);
    vc->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &dst);
    vc->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &target);
    D3D11_VIDEO_COLOR black{};
    black.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f};
    vc->VideoProcessorSetOutputBackgroundColor(processor_.Get(), TRUE, &black);
    // No auto-processing (denoise, edge enhancement...) that drivers may switch on by default.
    vc->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
}

ID3D11VideoProcessorInputView* Converter::inputView(ID3D11Texture2D* src) {
    if (src != inputFor_ || !inputView_) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputView_.Reset();
        DC_HR(videoDevice_->CreateVideoProcessorInputView(src, enumerator_.Get(), &desc, &inputView_));
        inputFor_ = src;
    }
    return inputView_.Get();
}

ID3D11VideoProcessorOutputView* Converter::outputView(ID3D11Texture2D* dst, UINT arrayIndex) {
    auto& view = outputViews_[{dst, arrayIndex}];
    if (!view) {
        D3D11_TEXTURE2D_DESC td{};
        dst->GetDesc(&td);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
        if (td.ArraySize > 1) {
            desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.FirstArraySlice = arrayIndex;
            desc.Texture2DArray.ArraySize = 1;
        } else {
            desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        }
        DC_HR(videoDevice_->CreateVideoProcessorOutputView(dst, enumerator_.Get(), &desc, &view));
    }
    return view.Get();
}

void Converter::convert(ID3D11Texture2D* src, ID3D11Texture2D* dst, UINT arrayIndex) {
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    if (!processor_ || sd.Width != inWidth_ || sd.Height != inHeight_) setup(sd.Width, sd.Height);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView(src);
    DC_HR(videoContext_->VideoProcessorBlt(processor_.Get(), outputView(dst, arrayIndex), 0, 1, &stream));
}

}  // namespace dc::capd
