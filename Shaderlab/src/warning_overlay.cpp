#include "warning_overlay.h"
#include <gdiplus.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")

using namespace Gdiplus;

static bool point_in_rect(int px, int py, const RECT &rc) {
    return (px >= rc.left && px <= rc.right && py >= rc.top && py <= rc.bottom);
}

WarningOverlay::WarningOverlay() = default;
WarningOverlay::~WarningOverlay() { shutdown(); }

bool WarningOverlay::initialize(ID3D11Device *device) {
    m_device = device;
    GdiplusStartupInput input;
    return GdiplusStartup(&m_gdiplus_token, &input, nullptr) == Ok;
}

void WarningOverlay::shutdown() {
    m_srv.Reset();
    m_texture.Reset();
    if (m_gdiplus_token) { GdiplusShutdown(m_gdiplus_token); m_gdiplus_token = 0; }
}

void WarningOverlay::set_active(bool active) {
    m_active = active;
    if (m_active && !m_sound_played) {
        PlaySoundW(L"SystemHand", nullptr, SND_ALIAS | SND_ASYNC);
        m_sound_played = true;
    }
}

void WarningOverlay::on_mouse_move(int x, int y) {
    m_mouse_x = x; m_mouse_y = y;
    m_hovered_button = point_in_rect(x, y, m_btn_download) ? 0
                     : point_in_rect(x, y, m_btn_dismiss)  ? 1
                     : -1;
}

bool WarningOverlay::on_mouse_click(int x, int y) {
    if (!is_active()) return false;
    if (point_in_rect(x, y, m_btn_download)) {
        ShellExecuteW(nullptr, L"open", L"https://reshade.me", nullptr, nullptr, SW_SHOW);
        return true;
    }
    if (point_in_rect(x, y, m_btn_dismiss)) { dismiss(); return true; }
    return false;
}

void WarningOverlay::on_key_down(WPARAM key) {
    if (!is_active()) return;
    if (key == VK_ESCAPE) dismiss();
    if (key == VK_RETURN) ShellExecuteW(nullptr, L"open", L"https://reshade.me", nullptr, nullptr, SW_SHOW);
}

void WarningOverlay::update_and_render(ID3D11DeviceContext *, uint32_t width, uint32_t height, float dt) {
    if (!is_active() || width == 0 || height == 0) return;
    m_fade_alpha = (std::min)(1.0f, m_fade_alpha + dt * 4.0f);
    render_gdiplus(width, height);
}

void WarningOverlay::render_gdiplus(uint32_t width, uint32_t height) {
    if (!m_device) return;

    if (!m_texture || m_tex_width != width || m_tex_height != height) {
        m_tex_width = width; m_tex_height = height;
        m_srv.Reset(); m_texture.Reset();

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_texture))) return;
        if (FAILED(m_device->CreateShaderResourceView(m_texture.Get(), nullptr, &m_srv))) return;
    }

    Bitmap bitmap(width, height, PixelFormat32bppARGB);
    Graphics g(&bitmap);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // dim the whole frame so the card pops
    BYTE a = static_cast<BYTE>(180.0f * m_fade_alpha);
    SolidBrush backdrop(Color(a, 0, 0, 0));
    g.FillRectangle(&backdrop, 0, 0, (INT)width, (INT)height);

    // centered card
    float cw = (std::min)(500.0f, width * 0.88f);
    float ch = 200.0f;
    float cx = (width  - cw) * 0.5f;
    float cy = (height - ch) * 0.5f;

    BYTE ca = static_cast<BYTE>(245.0f * m_fade_alpha);
    SolidBrush card_bg(Color(ca, 18, 18, 18));
    g.FillRectangle(&card_bg, RectF(cx, cy, cw, ch));

    Pen border(Color(static_cast<BYTE>(120.0f * m_fade_alpha), 80, 80, 80), 1.0f);
    g.DrawRectangle(&border, RectF(cx, cy, cw, ch));

    StringFormat sf_center;
    sf_center.SetAlignment(StringAlignmentCenter);
    sf_center.SetLineAlignment(StringAlignmentCenter);

    StringFormat sf_left;
    sf_left.SetAlignment(StringAlignmentNear);
    sf_left.SetLineAlignment(StringAlignmentNear);

    BYTE ta = static_cast<BYTE>(255.0f * m_fade_alpha);

    // title
    Font font_title(L"Segoe UI", 15, FontStyleBold, UnitPixel);
    SolidBrush white(Color(ta, 230, 230, 230));
    RectF title_rect(cx + 24.0f, cy + 24.0f, cw - 48.0f, 22.0f);
    g.DrawString(L"ReShade Not Found", -1, &font_title, title_rect, &sf_left, &white);

    // body copy, multi-line so it reads fine at 720p
    Font font_body(L"Segoe UI", 12, FontStyleRegular, UnitPixel);
    SolidBrush grey(Color(ta, 150, 150, 150));
    RectF body_rect(cx + 24.0f, cy + 54.0f, cw - 48.0f, 56.0f);
    g.DrawString(
        L"ShaderLab requires ReShade with Full Add-on Support.\n"
        L"Install it via reshade.me, select ShaderLab.exe,\n"
        L"then choose DirectX 10/11/12.",
        -1, &font_body, body_rect, &sf_left, &grey);

    // two buttons side by side, hover detection updates m_hovered_button
    float btn_h = 30.0f;
    float btn_y = cy + ch - btn_h - 18.0f;
    float btn1_w = 180.0f;
    float btn2_w = 80.0f;
    float gap = 8.0f;
    float total_w = btn1_w + btn2_w + gap;
    float btn1_x = cx + (cw - total_w) * 0.5f;
    float btn2_x = btn1_x + btn1_w + gap;

    bool h1 = (m_hovered_button == 0);
    bool h2 = (m_hovered_button == 1);

    // primary CTA -> opens reshade.me
    RectF r1(btn1_x, btn_y, btn1_w, btn_h);
    SolidBrush b1(Color(static_cast<BYTE>(220.0f * m_fade_alpha), h1 ? 80 : 55, h1 ? 80 : 55, h1 ? 80 : 55));
    g.FillRectangle(&b1, r1);
    Pen btn1_border(Color(static_cast<BYTE>(160.0f * m_fade_alpha), 120, 120, 120), 1.0f);
    g.DrawRectangle(&btn1_border, r1);
    Font font_btn(L"Segoe UI", 12, FontStyleRegular, UnitPixel);
    g.DrawString(L"Get ReShade (reshade.me)", -1, &font_btn, r1, &sf_center, &white);

    // dismiss, just hides the card. fuck this warning screen
    RectF r2(btn2_x, btn_y, btn2_w, btn_h);
    SolidBrush b2(Color(static_cast<BYTE>(180.0f * m_fade_alpha), h2 ? 40 : 28, h2 ? 40 : 28, h2 ? 40 : 28));
    g.FillRectangle(&b2, r2);
    Pen btn2_border(Color(static_cast<BYTE>(100.0f * m_fade_alpha), 70, 70, 70), 1.0f);
    g.DrawRectangle(&btn2_border, r2);
    SolidBrush dim(Color(ta, 130, 130, 130));
    g.DrawString(L"Dismiss", -1, &font_btn, r2, &sf_center, &dim);

    m_btn_download = { (LONG)btn1_x, (LONG)btn_y, (LONG)(btn1_x + btn1_w), (LONG)(btn_y + btn_h) };
    m_btn_dismiss  = { (LONG)btn2_x, (LONG)btn_y, (LONG)(btn2_x + btn2_w), (LONG)(btn_y + btn_h) };

    // gdi+ bitmap -> d3d texture, row by row since strides can differ
    BitmapData data = {};
    Rect lock_rect(0, 0, width, height);
    if (bitmap.LockBits(&lock_rect, ImageLockModeRead, PixelFormat32bppARGB, &data) == Ok) {
        ComPtr<ID3D11DeviceContext> ctx;
        m_device->GetImmediateContext(&ctx);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (ctx && SUCCEEDED(ctx->Map(m_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            for (uint32_t y = 0; y < height; ++y)
                memcpy((BYTE *)mapped.pData + y * mapped.RowPitch,
                       (const BYTE *)data.Scan0 + y * data.Stride, width * 4);
            ctx->Unmap(m_texture.Get(), 0);
        }
        bitmap.UnlockBits(&data);
    }
}
