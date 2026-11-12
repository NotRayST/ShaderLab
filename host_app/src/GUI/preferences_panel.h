#pragma once

#include "types.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <functional>
#include <string>

namespace gui {

struct AppPreferences {
    // 0=Photoshop Dark Gray, 1=Midnight OLED Black, 2=Nordic Slate,
    // 3=Cyberpunk Neon, 4=Light Studio, 5=Custom
    int theme_preset = 0;

    float col_window_bg[4] = { 0.149f, 0.149f, 0.149f, 1.0f };
    float col_apron_bg[4]  = { 0.098f, 0.098f, 0.098f, 1.0f };
    float col_frame_bg[4]  = { 0.118f, 0.118f, 0.118f, 1.0f };
    float col_accent[4]    = { 0.173f, 0.427f, 0.722f, 1.0f };
    float col_button[4]    = { 0.251f, 0.251f, 0.251f, 1.0f };
    float col_text[4]      = { 0.922f, 0.922f, 0.922f, 1.0f };
    float col_border[4]    = { 0.267f, 0.267f, 0.267f, 1.0f };

    float ui_rounding = 3.0f;
    float ui_border_size = 1.0f;

    // Custom UI textures: mode 0=Tile, 1=Stretch, 2=Center
    std::wstring panel_texture_path;
    int panel_texture_mode = 0;
    float panel_texture_opacity = 0.25f;
    float panel_texture_scale = 1.0f;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> panel_texture_srv;
    int panel_texture_w = 0;
    int panel_texture_h = 0;

    std::wstring apron_texture_path;
    int apron_texture_mode = 0;
    float apron_texture_opacity = 0.25f;
    float apron_texture_scale = 1.0f;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> apron_texture_srv;
    int apron_texture_w = 0;
    int apron_texture_h = 0;

    // Checkerboard transparency backdrop
    float checker_size = 16.0f;
    float checker_col_a[4] = { 0.80f, 0.80f, 0.80f, 1.0f };
    float checker_col_b[4] = { 0.55f, 0.55f, 0.55f, 1.0f };

    // Export & autosave defaults
    int default_export_format = 0; // 0=PNG, 1=JPG, 2=TGA
    int export_jpeg_quality = 92;
    int autosave_interval_min = 0; // 0=Off, 1, 5, 15
};

std::wstring get_default_preferences_path();

void apply_theme_preset(AppPreferences &prefs, int preset_index);
void apply_preferences_theme(const AppPreferences &prefs);

bool load_custom_ui_texture(
    ID3D11Device *device,
    const std::wstring &path,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> &out_srv,
    int &out_w,
    int &out_h
);

// Draws custom panel or canvas apron texture using ImGui draw_list (clipped).
// Call with ImGui::GetWindowDrawList() or background draw list, never foreground.
void draw_custom_panel_texture(
    ImDrawList *draw_list,
    const ImVec2 &p_min,
    const ImVec2 &p_max,
    ID3D11ShaderResourceView *srv,
    int tex_w,
    int tex_h,
    int mode,
    float opacity,
    float scale
);

inline void draw_custom_panel_texture(
    ImDrawList *draw_list,
    const ImVec2 &p_min,
    const ImVec2 &p_max,
    const AppPreferences &prefs,
    bool is_apron = false
) {
    if (is_apron) {
        draw_custom_panel_texture(
            draw_list, p_min, p_max,
            prefs.apron_texture_srv.Get(), prefs.apron_texture_w, prefs.apron_texture_h,
            prefs.apron_texture_mode, prefs.apron_texture_opacity, prefs.apron_texture_scale
        );
    } else {
        draw_custom_panel_texture(
            draw_list, p_min, p_max,
            prefs.panel_texture_srv.Get(), prefs.panel_texture_w, prefs.panel_texture_h,
            prefs.panel_texture_mode, prefs.panel_texture_opacity, prefs.panel_texture_scale
        );
    }
}

bool save_preferences_json(const AppPreferences &prefs, const std::wstring &path = L"");
bool load_preferences_json(ID3D11Device *device, AppPreferences &prefs, const std::wstring &path = L"");

// Renders the full Preferences & Customization tab (tab_prefs).
// Returns true if any visual/checkerboard/preference value changed this frame.
bool render_preferences_tab(
    HWND hwnd,
    ID3D11Device *device,
    AppPreferences &prefs,
    UiDomRegistry *dom,
    const std::function<void()> &on_changed = nullptr
);

} // namespace gui
