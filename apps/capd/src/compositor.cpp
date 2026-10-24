#include "compositor.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace dc::capd {

namespace {

// One textured quad per layer. The vertex shader makes the 4 corners from SV_VertexID, so there is no vertex buffer.
constexpr char kShader[] = R"(
cbuffer Layer : register(b0) {
    float4 dst;      // left, top, right, bottom in clip space
    float4 params;   // x = opacity
};
Texture2D tex : register(t0);
SamplerState smp : register(s0);

struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VsOut vs_main(uint id : SV_VertexID) {
    float2 c = float2(id & 1, id >> 1);  // (0,0) (1,0) (0,1) (1,1)
    VsOut o;
    o.pos = float4(lerp(dst.x, dst.z, c.x), lerp(dst.y, dst.w, c.y), 0, 1);
    o.uv = c;
    return o;
}

float4 ps_main(VsOut i) : SV_Target {
    float4 c = tex.Sample(smp, i.uv);
    return float4(c.rgb, params.x);  // desktop captures carry no meaningful alpha
}
)";

struct LayerConstants {
    float dst[4];
    float params[4];
};

ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(kShader, sizeof kShader - 1, "compositor", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        std::string msg = std::string("shader ") + entry;
        if (errors) msg += ": " + std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        throw std::runtime_error(msg);
    }
    return code;
}

}  // namespace

Compositor::Compositor(const Device& dev, UINT width, UINT height) : dev_(dev), width_(width), height_(height) {
    ID3D11Device* d = dev_.device.Get();
    canvas_ = createBgraTexture(d, width_, height_);
    DC_HR(d->CreateRenderTargetView(canvas_.Get(), nullptr, &rtv_));

    auto vsCode = compile("vs_main", "vs_5_0");
    auto psCode = compile("ps_main", "ps_5_0");
    DC_HR(d->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_));
    DC_HR(d->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps_));

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(LayerConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    DC_HR(d->CreateBuffer(&cb, nullptr, &constants_));

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    DC_HR(d->CreateSamplerState(&sd, &sampler_));

    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    DC_HR(d->CreateBlendState(&bd, &blend_));

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    DC_HR(d->CreateRasterizerState(&rd, &raster_));
}

ID3D11ShaderResourceView* Compositor::viewFor(ID3D11Texture2D* texture) {
    auto& view = views_[texture];
    if (!view) {
        // Sources recreate their texture when the monitor size changes; drop views of textures that are gone.
        if (views_.size() > 16) {
            views_.clear();
            return viewFor(texture);
        }
        DC_HR(dev_.device->CreateShaderResourceView(texture, nullptr, &view));
    }
    return view.Get();
}

LayerDraw Compositor::fit(ID3D11Texture2D* texture, UINT srcW, UINT srcH) const {
    LayerDraw l;
    l.texture = texture;
    if (srcW == 0 || srcH == 0) return l;
    const float scale = std::min(float(width_) / float(srcW), float(height_) / float(srcH));
    l.width = float(srcW) * scale;
    l.height = float(srcH) * scale;
    l.x = (float(width_) - l.width) / 2;
    l.y = (float(height_) - l.height) / 2;
    return l;
}

void Compositor::render(const std::vector<LayerDraw>& layers) {
    ID3D11DeviceContext* ctx = dev_.context.Get();
    const float black[4] = {0, 0, 0, 1};
    ctx->ClearRenderTargetView(rtv_.Get(), black);

    D3D11_VIEWPORT vp{0, 0, float(width_), float(height_), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(raster_.Get());
    ctx->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    ctx->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());

    for (const LayerDraw& l : layers) {
        if (!l.texture || l.width <= 0 || l.height <= 0 || l.opacity <= 0) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        DC_HR(ctx->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
        LayerConstants c{};
        c.dst[0] = l.x / float(width_) * 2 - 1;
        c.dst[1] = 1 - l.y / float(height_) * 2;
        c.dst[2] = (l.x + l.width) / float(width_) * 2 - 1;
        c.dst[3] = 1 - (l.y + l.height) / float(height_) * 2;
        c.params[0] = std::clamp(l.opacity, 0.0f, 1.0f);
        std::memcpy(m.pData, &c, sizeof c);
        ctx->Unmap(constants_.Get(), 0);

        ID3D11ShaderResourceView* srv = viewFor(l.texture);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->Draw(4, 0);
    }

    // Unbind so the source textures can be written by the capture sources again without hazards.
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

}  // namespace dc::capd
