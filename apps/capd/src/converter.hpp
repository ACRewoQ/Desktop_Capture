// BGRA -> NV12 conversion and scaling on the GPU with ID3D11VideoProcessor (fixed-function, no shaders, no CPU copy).
// It can also output BGRA (scaling only), for the Windows 10 virtual camera, which takes RGB.
#pragma once

#include "d3d.hpp"

#include <d3d11.h>

#include <map>
#include <utility>

namespace dc::capd {

class Converter {
public:
    Converter(const Device& dev, UINT outWidth, UINT outHeight, UINT fps, DXGI_FORMAT outFormat = DXGI_FORMAT_NV12);

    // Converts the whole of src into slice `arrayIndex` of the texture dst (NV12 or the format given above), scaled
    // to the output size, aspect ratio kept with black bars. NV12 is BT.709 limited range.
    void convert(ID3D11Texture2D* src, ID3D11Texture2D* dst, UINT arrayIndex);

private:
    void setup(UINT inWidth, UINT inHeight);
    ID3D11VideoProcessorInputView* inputView(ID3D11Texture2D* src);
    ID3D11VideoProcessorOutputView* outputView(ID3D11Texture2D* dst, UINT arrayIndex);

    Device dev_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    UINT outWidth_, outHeight_, fps_;
    DXGI_FORMAT outFormat_;
    UINT inWidth_ = 0, inHeight_ = 0;
    ID3D11Texture2D* inputFor_ = nullptr;
    ComPtr<ID3D11VideoProcessorInputView> inputView_;
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorOutputView>> outputViews_;
};

}  // namespace dc::capd
