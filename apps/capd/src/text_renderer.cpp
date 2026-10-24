#include "text_renderer.hpp"

#include <algorithm>
#include <cmath>

namespace dc::capd {

namespace {

D2D1_COLOR_F toD2d(std::uint32_t argb) {
    const auto c = argbToRgba(argb);
    return D2D1::ColorF(c.r, c.g, c.b, c.a);
}

}  // namespace

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

TextRenderer::TextRenderer(const Device& dev) : dev_(dev) {
    D2D1_FACTORY_OPTIONS opts{};
    DC_HR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                            reinterpret_cast<void**>(d2d_.GetAddressOf())));
    DC_HR(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())));
}

ComPtr<ID3D11Texture2D> TextRenderer::render(const TextStyle& style, UINT* width, UINT* height) {
    *width = *height = 0;
    const std::wstring text = widen(style.content);
    if (text.empty()) return nullptr;

    ComPtr<IDWriteTextFormat> format;
    DC_HR(dwrite_->CreateTextFormat(widen(style.font.empty() ? "Yu Gothic UI" : style.font).c_str(), nullptr,
                                    style.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                    style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(std::max(style.size, 1)), L"ja-jp",
                                    &format));
    format->SetTextAlignment(style.align == "center"  ? DWRITE_TEXT_ALIGNMENT_CENTER
                             : style.align == "right" ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                      : DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetWordWrapping(style.wrapWidth > 0 ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);

    constexpr float kMax = 8192.0f;
    ComPtr<IDWriteTextLayout> layout;
    DC_HR(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(),
                                    style.wrapWidth > 0 ? float(style.wrapWidth) : kMax, kMax, &layout));
    DWRITE_TEXT_METRICS m{};
    DC_HR(layout->GetMetrics(&m));
    // Without wrapping the layout box is the widest line, so centre / right alignment work between the lines.
    const float boxW = style.wrapWidth > 0 ? float(style.wrapWidth) : std::ceil(m.widthIncludingTrailingWhitespace);
    layout->SetMaxWidth(std::max(boxW, 1.0f));
    layout->SetMaxHeight(std::max(std::ceil(m.height), 1.0f));

    constexpr float kPad = 2.0f;
    const UINT w = static_cast<UINT>(std::clamp(boxW + 2 * kPad, 1.0f, kMax));
    const UINT h = static_cast<UINT>(std::clamp(std::ceil(m.height) + 2 * kPad, 1.0f, kMax));
    auto tex = createBgraTexture(dev_.device.Get(), w, h);

    ComPtr<IDXGISurface> surface;
    DC_HR(tex.As(&surface));
    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1RenderTarget> rt;
    DC_HR(d2d_->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &rt));
    // ClearType assumes an opaque background; text over video needs grayscale anti-aliasing.
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ComPtr<ID2D1SolidColorBrush> brush;
    DC_HR(rt->CreateSolidColorBrush(toD2d(style.color), &brush));
    rt->BeginDraw();
    rt->Clear(toD2d(style.background));
    rt->DrawTextLayout(D2D1::Point2F(kPad, kPad), layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    DC_HR(rt->EndDraw());

    *width = w;
    *height = h;
    return tex;
}

ID3D11ShaderResourceView* TextRenderer::noSignalView() {
    if (!noSignalView_) {
        TextStyle s;
        s.content = "NO SIGNAL\n信号なし";
        s.size = 64;
        s.color = 0xFFB0B0B0;
        s.background = 0xFF202020;
        s.align = "center";
        s.bold = true;
        noSignal_ = render(s, &noSignalW_, &noSignalH_);
        DC_HR(dev_.device->CreateShaderResourceView(noSignal_.Get(), nullptr, &noSignalView_));
    }
    return noSignalView_.Get();
}

}  // namespace dc::capd
