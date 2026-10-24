#include "compositor.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace dc::capd {

namespace {

// One textured (or solid) quad per layer. The vertex shader makes the 4 corners from SV_VertexID, so there is no
// vertex buffer. Output is premultiplied alpha; the blend state is ONE / INV_SRC_ALPHA.
// The chroma key follows OBS: distance of the pixel's CbCr from the key's CbCr; below `similarity` the pixel is
// keyed out, `smoothness` widens the edge, and `spill` desaturates what is left of the key colour.
constexpr char kShader[] = R"(
cbuffer Layer : register(b0) {
    float4 dst;      // left, top, right, bottom in clip space
    float4 uv;       // u0, v0, u1, v1
    float4 params;   // x = opacity, y = mode (0 opaque, 1 straight, 2 premultiplied, 3 solid), z = chroma on
    float4 color;    // solid colour, straight alpha
    float4 chroma;   // x, y = key Cb Cr; z = similarity; w = smoothness
    float4 chroma2;  // x = spill
};
Texture2D tex : register(t0);
SamplerState smp : register(s0);

struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VsOut vs_main(uint id : SV_VertexID) {
    float2 c = float2(id & 1, id >> 1);  // (0,0) (1,0) (0,1) (1,1)
    VsOut o;
    o.pos = float4(lerp(dst.x, dst.z, c.x), lerp(dst.y, dst.w, c.y), 0, 1);
    o.uv = lerp(uv.xy, uv.zw, c);
    return o;
}

float4 ps_main(VsOut i) : SV_Target {
    float4 c;
    if (params.y > 2.5) {
        c = float4(color.rgb * color.a, color.a);
    } else {
        c = tex.Sample(smp, i.uv);
        if (params.y < 0.5) c.a = 1;               // captures carry no meaningful alpha
        if (params.y < 1.5) c.rgb *= c.a;          // to premultiplied
    }
    if (params.z > 0.5) {
        float3 rgb = c.a > 0 ? c.rgb / c.a : float3(0, 0, 0);
        float2 cbcr = float2(dot(rgb, float3(-0.1146, -0.3854, 0.5)), dot(rgb, float3(0.5, -0.4542, -0.0458)));
        float base = distance(cbcr, chroma.xy) - chroma.z;
        float mask = saturate(base / max(chroma.w, 0.0001));
        float keep = pow(saturate(base / max(chroma2.x, 0.0001)), 1.5);
        float lum = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        rgb = lerp(float3(lum, lum, lum), rgb, keep);
        float a = c.a * mask;
        c = float4(rgb * a, a);
    }
    return c * params.x;
}
)";

struct LayerConstants {
    float dst[4];
    float uv[4];
    float params[4];
    float color[4];
    float chroma[4];
    float chroma2[4];
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

ComPtr<ID3D11Texture2D> createTarget(ID3D11Device* d, UINT w, UINT h, ComPtr<ID3D11RenderTargetView>* rtv,
                                     ComPtr<ID3D11ShaderResourceView>* srv) {
    auto tex = createBgraTexture(d, w, h);
    if (rtv) DC_HR(d->CreateRenderTargetView(tex.Get(), nullptr, rtv->ReleaseAndGetAddressOf()));
    if (srv) DC_HR(d->CreateShaderResourceView(tex.Get(), nullptr, srv->ReleaseAndGetAddressOf()));
    return tex;
}

}  // namespace

Compositor::Compositor(const Device& dev, UINT width, UINT height) : dev_(dev), width_(width), height_(height) {
    ID3D11Device* d = dev_.device.Get();
    canvas_ = createTarget(d, width_, height_, &canvasRtv_, nullptr);
    scratch_ = createTarget(d, width_, height_, &scratchRtv_, &scratchSrv_);

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
    rt.SrcBlend = D3D11_BLEND_ONE;
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

void Compositor::draw(const LayerDraw& l) {
    if ((!l.view && !l.solid) || l.width <= 0 || l.height <= 0 || l.opacity <= 0) return;
    ID3D11DeviceContext* ctx = dev_.context.Get();
    D3D11_MAPPED_SUBRESOURCE m{};
    DC_HR(ctx->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
    LayerConstants c{};
    c.dst[0] = l.x / float(width_) * 2 - 1;
    c.dst[1] = 1 - l.y / float(height_) * 2;
    c.dst[2] = (l.x + l.width) / float(width_) * 2 - 1;
    c.dst[3] = 1 - (l.y + l.height) / float(height_) * 2;
    c.uv[0] = l.u0;
    c.uv[1] = l.v0;
    c.uv[2] = l.u1;
    c.uv[3] = l.v1;
    c.params[0] = std::clamp(l.opacity, 0.0f, 1.0f);
    c.params[1] = l.solid ? 3.0f : float(l.alpha);
    c.params[2] = l.chroma ? 1.0f : 0.0f;
    c.color[0] = l.color.r;
    c.color[1] = l.color.g;
    c.color[2] = l.color.b;
    c.color[3] = l.color.a;
    if (l.chroma) {
        const auto k = argbToRgba(0xFF000000u | l.chromaColor);
        c.chroma[0] = -0.1146f * k.r - 0.3854f * k.g + 0.5f * k.b;
        c.chroma[1] = 0.5f * k.r - 0.4542f * k.g - 0.0458f * k.b;
        c.chroma[2] = l.similarity;
        c.chroma[3] = l.smoothness;
        c.chroma2[0] = l.spill;
    }
    std::memcpy(m.pData, &c, sizeof c);
    ctx->Unmap(constants_.Get(), 0);

    ID3D11ShaderResourceView* srv = l.view;
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->Draw(4, 0);
}

void Compositor::render(Target target, Rgba bg, const std::vector<LayerDraw>& layers) {
    ID3D11DeviceContext* ctx = dev_.context.Get();
    ID3D11RenderTargetView* rtv = target == Target::Canvas ? canvasRtv_.Get() : scratchRtv_.Get();
    const float clear[4] = {bg.r * bg.a, bg.g * bg.a, bg.b * bg.a, bg.a};
    ctx->ClearRenderTargetView(rtv, clear);

    D3D11_VIEWPORT vp{0, 0, float(width_), float(height_), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(raster_.Get());
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());

    for (const LayerDraw& l : layers) draw(l);

    // Unbind so the source textures can be written by the sources again without hazards.
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

void Compositor::blendScratchOverCanvas(float opacity) {
    ID3D11DeviceContext* ctx = dev_.context.Get();
    D3D11_VIEWPORT vp{0, 0, float(width_), float(height_), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(raster_.Get());
    ctx->OMSetRenderTargets(1, canvasRtv_.GetAddressOf(), nullptr);
    ctx->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());

    LayerDraw l;
    l.view = scratchSrv_.Get();
    l.alpha = 2;  // the scratch canvas is premultiplied
    l.width = float(width_);
    l.height = float(height_);
    l.opacity = opacity;
    draw(l);

    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

void Compositor::freeze() {
    if (!frozen_) frozen_ = createBgraTexture(dev_.device.Get(), width_, height_);
    dev_.context->CopyResource(frozen_.Get(), canvas_.Get());
}

void Compositor::restoreFrozen() {
    if (frozen_) dev_.context->CopyResource(canvas_.Get(), frozen_.Get());
}

}  // namespace dc::capd
