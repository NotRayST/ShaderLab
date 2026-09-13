#include "hud_renderer.h"
#include <cmath>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <filesystem>

namespace fs = std::filesystem;

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

static constexpr float kPi = 3.14159265358979323846f;

static const wchar_t *kWelcomeArt =
    LR"(
  ____   _               _            _             _     
 / ___| | |__   __ _  __| | ___ _ __ | |     __ _  | |__   
 \___ \ | '_ \ / _` |/ _` |/ _ \ '__|| |    /  _`|| ' _ \  
  ___) || | | | (_| | (_| |  __/ |   | |___ | (_|||| |_) | 
 |____/ |_| |_|\__,_|\__,_|\___|_|   |_____|\__,_||_.___/  
)";

void HudFontManager::initialize(const std::wstring &font_path_or_family) {
    if (m_initialized) return;
    m_initialized = true;

    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        fs::path exe_dir = fs::path(exe_path).parent_path();
        fs::path override_file = exe_dir / "common" / "fonts" / "active_font.ttf";
        if (fs::exists(override_file)) {
            set_font(override_file.wstring());
            return;
        }
    }

    set_font(font_path_or_family);
}

bool HudFontManager::set_font(const std::wstring &font_path_or_family) {
    fs::path p(font_path_or_family);
    std::vector<fs::path> candidates;

    if (p.has_extension() || p.string().find('/') != std::string::npos || p.string().find('\\') != std::string::npos) {
        candidates.push_back(p);
        wchar_t exe_path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
            fs::path exe_dir = fs::path(exe_path).parent_path();
            candidates.push_back(exe_dir / p);
            candidates.push_back(exe_dir / "common" / "fonts" / p.filename());
        }
        candidates.push_back(fs::current_path() / p);
        candidates.push_back(fs::current_path() / "common" / "fonts" / p.filename());
    }

    for (const auto &cand : candidates) {
        std::error_code ec;
        if (fs::exists(cand, ec)) {
            auto new_collection = std::make_unique<Gdiplus::PrivateFontCollection>();
            if (new_collection->AddFontFile(cand.c_str()) == Gdiplus::Ok) {
                int count = new_collection->GetFamilyCount();
                if (count > 0) {
                    Gdiplus::FontFamily family;
                    int found = 0;
                    new_collection->GetFamilies(1, &family, &found);
                    if (found > 0) {
                        wchar_t name[LF_FACESIZE] = {};
                        family.GetFamilyName(name);
                        m_font_name = name;
                        m_font_collection = std::move(new_collection);
                        m_is_custom = true;
                        return true;
                    }
                }
            }
        }
    }

    m_font_collection.reset();
    m_font_collection.reset();
    m_font_name = font_path_or_family;
    m_is_custom = false;
    return true;
}

std::unique_ptr<Gdiplus::Font> HudFontManager::create_font(float size_px, Gdiplus::FontStyle style) const {
    if (m_font_collection) {
        auto font = std::make_unique<Gdiplus::Font>(m_font_name.c_str(), size_px, style, Gdiplus::UnitPixel, m_font_collection.get());
        if (font->GetLastStatus() == Gdiplus::Ok) {
            return font;
        }
    }
    return std::make_unique<Gdiplus::Font>(m_font_name.c_str(), size_px, style, Gdiplus::UnitPixel);
}

GdiHudLayer::GdiHudLayer() = default;

GdiHudLayer::~GdiHudLayer() {
    shutdown();
}

bool GdiHudLayer::initialize(ID3D11Device *device) {
    m_device = device;
    return true;
}

void GdiHudLayer::shutdown() {
    m_srv.Reset();
    m_texture.Reset();
    m_device = nullptr;
}

bool GdiHudLayer::ensure_texture(uint32_t width, uint32_t height) {
    if (!m_device || width == 0 || height == 0) return false;

    if (!m_texture || m_tex_width != width || m_tex_height != height) {
        m_tex_width = width;
        m_tex_height = height;
        m_srv.Reset();
        m_texture.Reset();

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_texture);
        if (FAILED(hr)) return false;

        hr = m_device->CreateShaderResourceView(m_texture.Get(), nullptr, &m_srv);
        if (FAILED(hr)) return false;
    }
    return true;
}

void GdiHudLayer::upload_bitmap(ID3D11DeviceContext *context, Bitmap &bitmap, uint32_t width, uint32_t height) {
    if (!context || !m_texture || width == 0 || height == 0) return;

    BitmapData data = {};
    Rect lock_rect(0, 0, width, height);
    if (bitmap.LockBits(&lock_rect, ImageLockModeRead, PixelFormat32bppARGB, &data) == Ok) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context->Map(m_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            for (uint32_t y = 0; y < height; ++y) {
                memcpy((BYTE *)mapped.pData + y * mapped.RowPitch,
                       (const BYTE *)data.Scan0 + y * data.Stride,
                       width * 4);
            }
            context->Unmap(m_texture.Get(), 0);
        }
        bitmap.UnlockBits(&data);
    }
}

void CompassHud::update(
    float dt,
    bool is_rotating,
    float /*idle_seconds*/,
    float current_angle,
    bool is_snapped,
    bool is_fine,
    float center_x,
    float center_y,
    float img_screen_w,
    float img_screen_h,
    bool is_locked
) {
    m_angle = current_angle;
    m_is_snapped = is_snapped;
    m_is_fine = is_fine;
    m_is_locked = is_locked;
    m_center_x = center_x;
    m_center_y = center_y;
    m_img_w = img_screen_w;
    m_img_h = img_screen_h;

    if (is_rotating) {
        m_fade_alpha = (std::min)(1.0f, m_fade_alpha + dt * 6.0f);
    } else {
        m_fade_alpha = 0.0f;
    }
}

void CompassHud::render(ID3D11DeviceContext *context, uint32_t width, uint32_t height) {
    if (!context || !m_device || width == 0 || height == 0 || m_fade_alpha <= 0.001f) return;
    if (!ensure_texture(width, height)) return;

    Bitmap bitmap(width, height, PixelFormat32bppARGB);
    Graphics g(&bitmap);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    float cx = m_center_x;
    float cy = m_center_y;

    float max_r = (std::min)(static_cast<float>(width) * 0.35f, static_cast<float>(height) * 0.35f);
    float radius = std::clamp(125.0f, 75.0f, (std::max)(80.0f, max_r));

    BYTE fa = static_cast<BYTE>(255.0f * m_fade_alpha);

    BYTE disc_a = static_cast<BYTE>(60.0f * m_fade_alpha);
    SolidBrush disc_brush(Color(disc_a, 0, 0, 0));
    g.FillEllipse(&disc_brush, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
    BYTE ring_a = static_cast<BYTE>(100.0f * m_fade_alpha);
    Pen ring_pen(Color(ring_a, 80, 80, 80), 1.5f);
    g.DrawEllipse(&ring_pen, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

    float display_angle = std::fmod(m_angle, 360.0f);
    if (display_angle < 0.0f) display_angle += 360.0f;

    auto label_font = HudFontManager::get().create_font(10.0f, FontStyleRegular);
    StringFormat sf_center;
    sf_center.SetAlignment(StringAlignmentCenter);
    sf_center.SetLineAlignment(StringAlignmentCenter);

    for (int deg = 0; deg < 360; deg += 45) {
        float rad = static_cast<float>(deg) * (kPi / 180.0f);
        float dir_x = std::sin(rad);
        float dir_y = -std::cos(rad);

        bool is_cardinal = (deg % 90 == 0);
        float tick_len = is_cardinal ? 10.0f : 6.0f;

        float diff = std::abs(display_angle - static_cast<float>(deg));
        if (diff > 180.0f) diff = 360.0f - diff;
        bool is_near_snap = (diff <= 4.0f);

        BYTE tick_a = static_cast<BYTE>((is_near_snap ? 220.0f : (is_cardinal ? 160.0f : 100.0f)) * m_fade_alpha);
        BYTE tick_c = is_near_snap ? 230 : (is_cardinal ? 180 : 120);
        Pen tick_pen(Color(tick_a, tick_c, tick_c, tick_c), is_cardinal ? 2.0f : 1.5f);

        float p1_x = cx + dir_x * (radius - tick_len);
        float p1_y = cy + dir_y * (radius - tick_len);
        float p2_x = cx + dir_x * radius;
        float p2_y = cy + dir_y * radius;
        g.DrawLine(&tick_pen, p1_x, p1_y, p2_x, p2_y);

        float label_dist = radius - tick_len - 12.0f;
        float lx = cx + dir_x * label_dist;
        float ly = cy + dir_y * label_dist;

        BYTE text_a = static_cast<BYTE>((is_near_snap ? 255.0f : 150.0f) * m_fade_alpha);
        BYTE text_c = is_near_snap ? 230 : 150;
        SolidBrush text_brush(Color(text_a, text_c, text_c, text_c));

        std::wstring label_str = std::to_wstring(deg) + L"°";
        RectF label_box(lx - 16.0f, ly - 8.0f, 32.0f, 16.0f);
        g.DrawString(label_str.c_str(), -1, label_font.get(), label_box, &sf_center, &text_brush);
    }

    float needle_rad = display_angle * (kPi / 180.0f);
    float ndir_x = std::sin(needle_rad);
    float ndir_y = -std::cos(needle_rad);

    float needle_len = radius - 4.0f;
    float n_tip_x = cx + ndir_x * needle_len;
    float n_tip_y = cy + ndir_y * needle_len;

    BYTE needle_a = static_cast<BYTE>(240.0f * m_fade_alpha);
    Pen needle_pen(Color(needle_a, 230, 230, 230), 2.0f);
    g.DrawLine(&needle_pen, cx, cy, n_tip_x, n_tip_y);

    SolidBrush tip_brush(Color(needle_a, 230, 230, 230));
    g.FillEllipse(&tip_brush, n_tip_x - 3.5f, n_tip_y - 3.5f, 7.0f, 7.0f);

    float card_w = m_is_locked ? 116.0f : (m_is_fine ? 92.0f : 74.0f);
    float card_h = 28.0f;
    float card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;

    BYTE ca = static_cast<BYTE>(245.0f * m_fade_alpha);
    SolidBrush card_bg(Color(ca, 18, 18, 18));
    g.FillRectangle(&card_bg, RectF(card_x, card_y, card_w, card_h));

    Pen border_pen(Color(static_cast<BYTE>(140.0f * m_fade_alpha), 80, 80, 80), 1.0f);
    g.DrawRectangle(&border_pen, RectF(card_x, card_y, card_w, card_h));

    std::wstringstream ss;
    if (std::abs(display_angle - std::round(display_angle)) < 0.01f) {
        ss << static_cast<int>(std::round(display_angle)) << L"°";
    } else {
        ss << std::fixed << std::setprecision(1) << display_angle << L"°";
    }

    auto card_font = HudFontManager::get().create_font(12.0f, FontStyleBold);
    SolidBrush card_text_brush(Color(fa, 230, 230, 230));

    if (m_is_locked) {
        RectF angle_box(card_x + 4.0f, card_y, 44.0f, card_h);
        g.DrawString(ss.str().c_str(), -1, card_font.get(), angle_box, &sf_center, &card_text_brush);

        auto lock_font = HudFontManager::get().create_font(9.0f, FontStyleBold);
        SolidBrush lock_brush(Color(static_cast<BYTE>(200.0f * m_fade_alpha), 200, 200, 200));
        RectF lock_box(card_x + 48.0f, card_y, 64.0f, card_h);
        g.DrawString(L"[LOCKED]", -1, lock_font.get(), lock_box, &sf_center, &lock_brush);
    } else if (m_is_fine) {
        RectF angle_box(card_x + 4.0f, card_y, 48.0f, card_h);
        g.DrawString(ss.str().c_str(), -1, card_font.get(), angle_box, &sf_center, &card_text_brush);

        auto fine_font = HudFontManager::get().create_font(9.0f, FontStyleRegular);
        SolidBrush fine_brush(Color(static_cast<BYTE>(180.0f * m_fade_alpha), 160, 160, 160));
        RectF fine_box(card_x + 50.0f, card_y, 38.0f, card_h);
        g.DrawString(L"FINE", -1, fine_font.get(), fine_box, &sf_center, &fine_brush);
    } else {
        RectF angle_box(card_x, card_y, card_w, card_h);
        g.DrawString(ss.str().c_str(), -1, card_font.get(), angle_box, &sf_center, &card_text_brush);
    }

    upload_bitmap(context, bitmap, width, height);
}

void ZoomHud::update(float dt, float zoom, float idle_seconds, bool is_locked, bool is_fine) {
    m_zoom = zoom;
    m_is_locked = is_locked;
    m_is_fine = is_fine;

    constexpr float kHoldSeconds = 1.5f;
    bool should_show = (std::abs(zoom - 1.0f) > 0.005f) || (idle_seconds < kHoldSeconds);
    if (should_show) {
        m_fade_alpha = (std::min)(1.0f, m_fade_alpha + dt * 5.0f);
    } else {
        m_fade_alpha = (std::max)(0.0f, m_fade_alpha - dt * 3.0f);
    }
}

void ZoomHud::render(ID3D11DeviceContext *context, uint32_t width, uint32_t height) {
    if (!context || !m_device || width == 0 || height == 0 || m_fade_alpha <= 0.001f) return;
    if (!ensure_texture(width, height)) return;

    Bitmap bitmap(width, height, PixelFormat32bppARGB);
    Graphics g(&bitmap);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    float chip_w = m_is_locked ? 165.0f : (m_is_fine ? 142.0f : 110.0f);
    float chip_h = 30.0f;
    float margin = 16.0f;
    float x = static_cast<float>(width) - chip_w - margin;
    float y = static_cast<float>(height) - chip_h - margin;

    BYTE ca = static_cast<BYTE>(245.0f * m_fade_alpha);
    SolidBrush bg_brush(Color(ca, 18, 18, 18));
    g.FillRectangle(&bg_brush, RectF(x, y, chip_w, chip_h));

    Pen border_pen(Color(static_cast<BYTE>(120.0f * m_fade_alpha), 80, 80, 80), 1.0f);
    g.DrawRectangle(&border_pen, RectF(x, y, chip_w, chip_h));

    int percent = static_cast<int>(std::round(m_zoom * 100.0f));

    auto font = HudFontManager::get().create_font(12.0f, FontStyleRegular);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);

    BYTE ta = static_cast<BYTE>(230.0f * m_fade_alpha);
    SolidBrush text_brush(Color(ta, 230, 230, 230));

    if (m_is_locked) {
        std::wstring str = L"zoom " + std::to_wstring(percent) + L"% [LOCKED]";
        RectF text_rect(x, y, chip_w, chip_h);
        g.DrawString(str.c_str(), -1, font.get(), text_rect, &sf, &text_brush);
    } else if (m_is_fine) {
        std::wstring str = L"zoom " + std::to_wstring(percent) + L"%";
        RectF text_rect(x + 4.0f, y, chip_w - 44.0f, chip_h);
        g.DrawString(str.c_str(), -1, font.get(), text_rect, &sf, &text_brush);

        auto fine_font = HudFontManager::get().create_font(9.0f, FontStyleRegular);
        SolidBrush fine_brush(Color(static_cast<BYTE>(180.0f * m_fade_alpha), 160, 160, 160));
        RectF fine_box(x + chip_w - 40.0f, y, 36.0f, chip_h);
        g.DrawString(L"FINE", -1, fine_font.get(), fine_box, &sf, &fine_brush);
    } else {
        std::wstring str = L"zoom " + std::to_wstring(percent) + L"%";
        RectF text_rect(x, y, chip_w, chip_h);
        g.DrawString(str.c_str(), -1, font.get(), text_rect, &sf, &text_brush);
    }

    upload_bitmap(context, bitmap, width, height);
}

void ToastHud::show(const std::wstring &text, float duration_s) {
    m_held = false;
    m_text = text;
    m_timer = duration_s;
}

void ToastHud::show_held(const std::wstring &text) {
    m_held = true;
    m_text = text;
    m_timer = 0.0f;
    m_fade_alpha = 1.0f;
}

void ToastHud::release_held() {
    if (m_held) {
        m_held = false;
        m_timer = 0.0f;
        m_fade_alpha = 0.0f;
    }
}

void ToastHud::update(float dt) {
    if (m_held) {
        m_fade_alpha = 1.0f;
        return;
    }
    if (m_timer > 0.0f) {
        m_timer -= dt;
        m_fade_alpha = (std::min)(1.0f, m_fade_alpha + dt * 8.0f);
    } else {
        m_fade_alpha = (std::max)(0.0f, m_fade_alpha - dt * 4.0f);
    }
}

void ToastHud::render(ID3D11DeviceContext *context, uint32_t width, uint32_t height) {
    if (!context || !m_device || width == 0 || height == 0 || m_fade_alpha <= 0.001f || m_text.empty()) return;
    if (!ensure_texture(width, height)) return;

    Bitmap bitmap(width, height, PixelFormat32bppARGB);
    Graphics g(&bitmap);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    auto font = HudFontManager::get().create_font(12.0f, FontStyleRegular);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);

    RectF layout_rect(0, 0, static_cast<float>(width), 100.0f);
    RectF bounds;
    g.MeasureString(m_text.c_str(), -1, font.get(), layout_rect, &sf, &bounds);

    float card_w = (std::max)(130.0f, bounds.Width + 36.0f);
    float card_h = 30.0f;
    float card_x = (static_cast<float>(width) - card_w) * 0.5f;
    float card_y = 20.0f;

    BYTE ca = static_cast<BYTE>(245.0f * m_fade_alpha);
    SolidBrush bg_brush(Color(ca, 18, 18, 18));
    g.FillRectangle(&bg_brush, RectF(card_x, card_y, card_w, card_h));

    Pen border_pen(Color(static_cast<BYTE>(120.0f * m_fade_alpha), 80, 80, 80), 1.0f);
    g.DrawRectangle(&border_pen, RectF(card_x, card_y, card_w, card_h));

    BYTE ta = static_cast<BYTE>(230.0f * m_fade_alpha);
    SolidBrush text_brush(Color(ta, 230, 230, 230));
    RectF text_rect(card_x, card_y, card_w, card_h);
    g.DrawString(m_text.c_str(), -1, font.get(), text_rect, &sf, &text_brush);

    upload_bitmap(context, bitmap, width, height);
}

static const wchar_t *kShowerEasterEggs[] = {
    L"slDp",
    L"SHLV",
    L"IPC_v9",
    L"D3D11",
    L"ReShade",
    L"WYSIWYG",
    L"NotRayST",
    L"DepthCanvas",
    L"SharedMemory",
    L"ShaderLab",
    L"0x53484C56",
    L"RTGI",
    L"LinearDepth",
};
static constexpr size_t kNumShowerEasterEggs = sizeof(kShowerEasterEggs) / sizeof(kShowerEasterEggs[0]);

static const wchar_t *kShowerGlyphs = L"01xyzrgbafuvw+-*/#~<>:;._";
static constexpr size_t kNumShowerGlyphs = 25;

WelcomeHud::WelcomeHud() = default;
WelcomeHud::~WelcomeHud() = default;

bool WelcomeHud::initialize(ID3D11Device *device) {
    if (!GdiHudLayer::initialize(device)) return false;
    HudFontManager::get().initialize();
    return true;
}

void WelcomeHud::update_shower(float dt, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;

    size_t num_cols = (width / 90) + 1;
    if (m_shower_cols.size() != num_cols || m_last_width != width || m_last_height != height) {
        m_last_width = width;
        m_last_height = height;
        m_shower_cols.resize(num_cols);
        for (size_t c = 0; c < num_cols; ++c) {
            auto &col = m_shower_cols[c];
            col.x = static_cast<float>(c) * 90.0f + 45.0f;
            col.speed = 22.0f + static_cast<float>(rand() % 14);
            col.mutate_timer = 0.0f;
            col.is_egg = false;

            int len = 4 + (rand() % 4);
            col.text.clear();
            for (int k = 0; k < len; ++k) {
                col.text.push_back(kShowerGlyphs[rand() % kNumShowerGlyphs]);
            }
            col.y = static_cast<float>(rand() % (height + 200)) - 100.0f;
        }

        if (num_cols >= 5 && (rand() % 2 == 0)) {
            size_t egg_col = (rand() % 2 == 0) ? 1 : (num_cols - 2);
            auto &col = m_shower_cols[egg_col];
            col.is_egg = true;
            col.text = kShowerEasterEggs[rand() % kNumShowerEasterEggs];
            col.y = static_cast<float>(rand() % (height / 3)) - 30.0f;
        }
    }

    for (auto &col : m_shower_cols) {
        col.y += col.speed * dt;

        if (!col.is_egg && !col.text.empty()) {
            col.mutate_timer += dt;
            if (col.mutate_timer >= 0.12f) {
                col.mutate_timer = 0.0f;
                col.text[rand() % col.text.size()] = kShowerGlyphs[rand() % kNumShowerGlyphs];
            }
        }

        // reset when the top letter falls off the bottom of the screen
        if (col.y > static_cast<float>(height) + 30.0f) {
            col.y = - static_cast<float>(rand() % 160) - 60.0f;
            col.speed = 22.0f + static_cast<float>(rand() % 14);
            col.mutate_timer = 0.0f;

            int active_eggs = 0;
            for (const auto &other : m_shower_cols) {
                if (other.is_egg && other.y < static_cast<float>(height) && other.y > -100.0f) {
                    active_eggs++;
                }
            }

            if (active_eggs == 0 && (rand() % 10 == 0)) {
                col.is_egg = true;
                col.text = kShowerEasterEggs[rand() % kNumShowerEasterEggs];
            } else {
                col.is_egg = false;
                int len = 4 + (rand() % 4);
                col.text.clear();
                for (int k = 0; k < len; ++k) {
                    col.text.push_back(kShowerGlyphs[rand() % kNumShowerGlyphs]);
                }
            }
        }
    }
}

void WelcomeHud::render_shower(Graphics &g, uint32_t width, uint32_t height) {
    (void)width;
    if (m_shower_cols.empty() || m_fade_alpha <= 0.001f) return;

    Font shower_font(L"Consolas", 13, FontStyleRegular, UnitPixel);
    Font egg_font(L"Consolas", 13, FontStyleBold, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);

    float char_spacing = 17.0f;
    for (const auto &col : m_shower_cols) {
        size_t len = col.text.size();
        if (len == 0) continue;

        for (size_t i = 0; i < len; ++i) {
            float cy = col.y + static_cast<float>(i) * char_spacing;
            if (cy < -20.0f || cy > static_cast<float>(height) + 20.0f) continue;

            float t = (len > 1) ? (static_cast<float>(i) / static_cast<float>(len - 1)) : 1.0f;
            float ramp = t * t;
            bool is_head = (i == len - 1);

            BYTE a = 0;
            Color char_color;
            Font *font_to_use = &shower_font;

            if (col.is_egg) {
                float egg_alpha = is_head ? (80.0f * m_fade_alpha) : ((8.0f + ramp * 55.0f) * m_fade_alpha);
                a = static_cast<BYTE>(std::clamp(egg_alpha, 0.0f, 255.0f));
                char_color = is_head ? Color(a, 205, 230, 245) : Color(a, 175, 200, 220);
            } else {
                float noise_alpha = is_head ? (50.0f * m_fade_alpha) : ((4.0f + ramp * 24.0f) * m_fade_alpha);
                a = static_cast<BYTE>(std::clamp(noise_alpha, 0.0f, 255.0f));
                if (is_head) {
                    char_color = Color(a, 215, 220, 230);
                } else {
                    char_color = Color(a, 140, 145, 155);
                }
            }

            if (a < 2) continue;

            SolidBrush brush(char_color);
            wchar_t str[2] = { col.text[i], L'\0' };
            RectF rect(col.x - 14.0f, cy - 8.0f, 28.0f, 18.0f);
            g.DrawString(str, 1, font_to_use, rect, &sf, &brush);
        }
    }
}

void WelcomeHud::update(float dt, bool has_image) {
    m_last_dt = dt;

    if (has_image) {
        m_fade_alpha = (std::max)(0.0f, m_fade_alpha - dt * 4.0f);
        return;
    }

    m_fade_alpha = (std::min)(1.0f, m_fade_alpha + dt * 2.0f);
}

void WelcomeHud::render(ID3D11DeviceContext *context, uint32_t width, uint32_t height) {
    if (!context || !m_device || width == 0 || height == 0 || m_fade_alpha <= 0.001f) return;
    if (!ensure_texture(width, height)) return;

    Bitmap bitmap(width, height, PixelFormat32bppARGB);
    Graphics g(&bitmap);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    update_shower(m_last_dt, width, height);
    render_shower(g, width, height);

    float cx = static_cast<float>(width) * 0.5f;
    float cy = static_cast<float>(height) * 0.5f;

    BYTE fa = static_cast<BYTE>(255.0f * m_fade_alpha);

    // responsive font size scaling for ascii logo so its not tiny on big screens
    float font_by_w = (static_cast<float>(width) * 0.65f) / (58.0f * 0.55f);
    float font_by_h = (static_cast<float>(height) * 0.22f) / 6.0f;
    float base_font_size = std::clamp((std::min)(font_by_w, font_by_h), 14.0f, 42.0f);
    Font title_font(L"Consolas", base_font_size, FontStyleBold, UnitPixel);

    StringFormat left_sf;
    left_sf.SetAlignment(StringAlignmentNear);
    left_sf.SetLineAlignment(StringAlignmentNear);

    StringFormat center_sf;
    center_sf.SetAlignment(StringAlignmentCenter);
    center_sf.SetLineAlignment(StringAlignmentCenter);

    std::wstring full(kWelcomeArt);
    int total = static_cast<int>(full.length());

    // measure the full block so it can be centered as one unit
    RectF measure_layout(0, 0, 9999.0f, 9999.0f);
    RectF block_bounds;
    g.MeasureString(full.c_str(), total, &title_font, measure_layout, &left_sf, &block_bounds);
    float block_left = cx - block_bounds.Width * 0.5f;
    float block_top  = cy - block_bounds.Height * 0.5f;

    SolidBrush text_brush(Color(fa, 235, 235, 235));
    RectF block_rect(block_left, block_top, block_bounds.Width, block_bounds.Height);
    g.DrawString(full.c_str(), total, &title_font, block_rect, &left_sf, &text_brush);

    {
        float ver_font_size = std::clamp(base_font_size * 0.38f, 10.0f, 14.0f);
        auto version_font = HudFontManager::get().create_font(ver_font_size, FontStyleRegular);
        BYTE va = static_cast<BYTE>(120.0f * m_fade_alpha);
        SolidBrush ver_brush(Color(va, 130, 130, 135));
        RectF ver_rect(0, static_cast<float>(height) - (ver_font_size * 2.5f + 8.0f), static_cast<float>(width), ver_font_size * 2.0f);
        g.DrawString(L"V1.2.1 - By NotRayST", -1, version_font.get(), ver_rect, &center_sf, &ver_brush);
    }

    upload_bitmap(context, bitmap, width, height);
}
