#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <gdiplus.h>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// app-wide font: family name, or a ttf path like L"common/fonts/ProggyClean.ttf"
static const wchar_t *kGlobalAppFont = L"Segoe UI";

class HudFontManager {
public:
    static HudFontManager &get() {
        static HudFontManager instance;
        return instance;
    }

    void initialize(const std::wstring &font_path_or_family = kGlobalAppFont);
    bool set_font(const std::wstring &font_path_or_family);

    std::unique_ptr<Gdiplus::Font> create_font(float size_px, Gdiplus::FontStyle style = Gdiplus::FontStyleRegular) const;
    const std::wstring &get_font_name() const { return m_font_name; }
    bool is_custom_font() const { return m_is_custom; }

private:
    HudFontManager() = default;
    ~HudFontManager() = default;

    std::wstring m_font_name = L"Segoe UI";
    std::unique_ptr<Gdiplus::PrivateFontCollection> m_font_collection;
    bool m_is_custom = false;
    bool m_initialized = false;
};



class GdiHudLayer {
public:
    GdiHudLayer();
    virtual ~GdiHudLayer();

    virtual bool initialize(ID3D11Device *device);
    virtual void shutdown();

    ID3D11ShaderResourceView *get_srv() const { return m_srv.Get(); }
    uintptr_t get_srv_handle() const { return reinterpret_cast<uintptr_t>(m_srv.Get()); }
    uint32_t get_width() const { return m_tex_width; }
    uint32_t get_height() const { return m_tex_height; }
    float get_fade_alpha() const { return m_fade_alpha; }

protected:
    bool ensure_texture(uint32_t width, uint32_t height);
    void upload_bitmap(ID3D11DeviceContext *context, Gdiplus::Bitmap &bitmap, uint32_t width, uint32_t height);

    ID3D11Device *m_device = nullptr;
    ComPtr<ID3D11Texture2D> m_texture;
    ComPtr<ID3D11ShaderResourceView> m_srv;

    uint32_t m_tex_width = 0;
    uint32_t m_tex_height = 0;
    float m_fade_alpha = 0.0f;
};

class CompassHud : public GdiHudLayer {
public:
    CompassHud() = default;
    ~CompassHud() override = default;

    void update(float dt, bool is_rotating, float idle_seconds, float current_angle, bool is_snapped, bool is_fine, float center_x, float center_y, float img_screen_w, float img_screen_h, bool is_locked = false);
    void render(ID3D11DeviceContext *context, uint32_t width, uint32_t height);
    bool is_visible() const { return m_fade_alpha > 0.001f; }

private:
    float m_angle = 0.0f;
    bool m_is_snapped = false;
    bool m_is_fine = false;
    bool m_is_locked = false;
    float m_center_x = 0.0f;
    float m_center_y = 0.0f;
    float m_img_w = 0.0f;
    float m_img_h = 0.0f;
};

class ZoomHud : public GdiHudLayer {
public:
    ZoomHud() = default;
    ~ZoomHud() override = default;

    void update(float dt, float zoom, float idle_seconds, bool is_locked = false, bool is_fine = false);
    void render(ID3D11DeviceContext *context, uint32_t width, uint32_t height);
    bool is_visible() const { return m_fade_alpha > 0.001f; }

private:
    float m_zoom = 1.0f;
    bool m_is_locked = false;
    bool m_is_fine = false;
};

class ToastHud : public GdiHudLayer {
public:
    ToastHud() = default;
    ~ToastHud() override = default;

    void show(const std::wstring &text, float duration_s = 1.4f);
    void show_held(const std::wstring &text);
    void release_held();
    void update(float dt);
    void render(ID3D11DeviceContext *context, uint32_t width, uint32_t height);
    bool is_visible() const { return m_fade_alpha > 0.001f; }

private:
    std::wstring m_text;
    float m_timer = 0.0f;
    bool m_held = false;
};

class WelcomeHud : public GdiHudLayer {
public:
    WelcomeHud();
    ~WelcomeHud() override;

    bool initialize(ID3D11Device *device) override;
    void update(float dt, bool has_image);
    void render(ID3D11DeviceContext *context, uint32_t width, uint32_t height);
    bool is_visible() const { return m_fade_alpha > 0.001f; }

private:
    struct ShowerColumn {
        float x = 0.0f;
        float y = 0.0f;
        float speed = 25.0f;
        float mutate_timer = 0.0f;
        std::wstring text;
        bool is_egg = false;
    };

    void update_shower(float dt, uint32_t width, uint32_t height);
    void render_shower(Gdiplus::Graphics &g, uint32_t width, uint32_t height);

    std::vector<ShowerColumn> m_shower_cols;
    uint32_t m_last_width = 0;
    uint32_t m_last_height = 0;
    float m_last_dt = 0.016f;
};
