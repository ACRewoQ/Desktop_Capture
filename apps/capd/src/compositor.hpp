// GPU compositor: draws the layers of one scene into the mix's BGRA canvas with a small D3D11 shader. Nothing
// is copied to the CPU. P2 has one scene with one layer (a monitor fitted into the canvas); scenes, transitions
// and the layer editing of the design doc are added in P3 (lane B1) on top of this.
#pragma once

#include "d3d.hpp"

#include <map>
#include <vector>

namespace dc::capd {

struct LayerDraw {
    ID3D11Texture2D* texture = nullptr;  // BGRA source image; null = layer is transparent (source lost)
    // Destination rectangle in canvas pixels.
    float x = 0, y = 0, width = 0, height = 0;
    float opacity = 1.0f;
};

class Compositor {
public:
    Compositor(const Device& dev, UINT width, UINT height);

    // Clears the canvas to black and draws the layers in order (back to front).
    void render(const std::vector<LayerDraw>& layers);

    ID3D11Texture2D* canvas() const { return canvas_.Get(); }
    UINT width() const { return width_; }
    UINT height() const { return height_; }

    // Destination rectangle that fits a source of srcW x srcH into the canvas, keeping the aspect ratio.
    LayerDraw fit(ID3D11Texture2D* texture, UINT srcW, UINT srcH) const;

private:
    ID3D11ShaderResourceView* viewFor(ID3D11Texture2D* texture);

    Device dev_;
    UINT width_, height_;
    ComPtr<ID3D11Texture2D> canvas_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11RasterizerState> raster_;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> views_;
};

}  // namespace dc::capd
