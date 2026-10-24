// GPU compositor of one video mix: draws the layers of a scene into the mix's canvas with a small D3D11 shader
// (crop through texture coordinates, opacity, chroma key, single colours), and the fade between two scenes.
// Nothing is copied to the CPU. The canvas holds premultiplied BGRA.
#pragma once

#include "d3d.hpp"
#include "video_config.hpp"

#include <vector>

namespace dc::capd {

struct LayerDraw {
    ID3D11ShaderResourceView* view = nullptr;  // null and !solid = nothing to draw
    bool solid = false;                        // fill with `color` instead of a texture
    Rgba color;                                // solid colour (straight alpha)
    int alpha = 0;                             // 0 = opaque source, 1 = straight alpha, 2 = premultiplied
    // Destination rectangle in canvas pixels and the shown part of the texture.
    float x = 0, y = 0, width = 0, height = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    float opacity = 1.0f;
    bool chroma = false;
    std::uint32_t chromaColor = 0x00FF00;
    float similarity = 0.4f, smoothness = 0.08f, spill = 0.1f;
};

class Compositor {
public:
    Compositor(const Device& dev, UINT width, UINT height);

    enum class Target { Canvas, Scratch };

    // Clears the target to the background colour and draws the layers in order (back to front).
    void render(Target target, Rgba background, const std::vector<LayerDraw>& layers);
    // Draws the scratch picture over the canvas with the given opacity (the fade).
    void blendScratchOverCanvas(float opacity);
    // Keeps a copy of the canvas / puts the copy back (a transition interrupted by another one).
    void freeze();
    void restoreFrozen();

    ID3D11Texture2D* canvas() const { return canvas_.Get(); }
    UINT width() const { return width_; }
    UINT height() const { return height_; }

private:
    void draw(const LayerDraw& l);

    Device dev_;
    UINT width_, height_;
    ComPtr<ID3D11Texture2D> canvas_, scratch_, frozen_;
    ComPtr<ID3D11RenderTargetView> canvasRtv_, scratchRtv_;
    ComPtr<ID3D11ShaderResourceView> scratchSrv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11RasterizerState> raster_;
};

}  // namespace dc::capd
