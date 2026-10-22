// BGRA -> NV12 conversion and scaling on the GPU with ID3D11VideoProcessor (fixed-function, no shaders, no CPU copy).
#pragma once

#include "d3d.hpp"

#include <d3d11.h>

#include <map>
#include <utility>

namespace dc::capbench {

class Converter {
public:
    Converter(const Device& dev, UINT outWidth, UINT outHeight, UINT fps);

    // Converts the whole of src into slice `arrayIndex` of the NV12 texture dst (scaled to the output size,
    // aspect ratio kept with black bars). BT.709 limited range.
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
    UINT inWidth_ = 0, inHeight_ = 0;
    ID3D11Texture2D* inputFor_ = nullptr;
    ComPtr<ID3D11VideoProcessorInputView> inputView_;
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorOutputView>> outputViews_;
};

}  // namespace dc::capbench
