// Direct2D / DirectWrite drawing into textures of the mixer's device: text sources and the "no signal" picture.
// Video thread only (single-threaded Direct2D factory).
#pragma once

#include "d3d.hpp"
#include "video_config.hpp"

#include <d2d1_1.h>
#include <dwrite.h>

namespace dc::capd {

std::wstring widen(const std::string& utf8);

class TextRenderer {
public:
    explicit TextRenderer(const Device& dev);

    // Draws the text into a new premultiplied BGRA texture just large enough for it. Null for empty text.
    ComPtr<ID3D11Texture2D> render(const TextStyle& style, UINT* width, UINT* height);

    // Shown by layers set to no_signal = image while their source has no picture.
    ID3D11ShaderResourceView* noSignalView();
    UINT noSignalWidth() const { return noSignalW_; }
    UINT noSignalHeight() const { return noSignalH_; }

private:
    Device dev_;
    ComPtr<ID2D1Factory1> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID3D11Texture2D> noSignal_;
    ComPtr<ID3D11ShaderResourceView> noSignalView_;
    UINT noSignalW_ = 0, noSignalH_ = 0;
};

}  // namespace dc::capd
