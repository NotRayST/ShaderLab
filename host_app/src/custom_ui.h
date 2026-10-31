#pragma once
#include <windows.h>
#include <d3d11.h>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

struct ShaderLabUniform {
    enum class Type { Float, Int, Bool, Color3, Color4, Enum };
    std::string name;
    std::string label;
    std::string tooltip;
    std::string category;
    Type type = Type::Float;
    float min_val = 0.0f;
    float max_val = 1.0f;
    float step = 0.01f;
    float float_val = 0.5f;
    float default_float = 0.5f;
    int int_val = 0;
    int default_int = 0;
    bool bool_val = false;
    bool default_bool = false;
    float color_val[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float default_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::vector<std::string> enum_items;
    int enum_selected = 0;
    int default_enum = 0;

    void reset_to_default() {
        float_val = default_float;
        int_val = default_int;
        bool_val = default_bool;
        for (int i = 0; i < 4; ++i) color_val[i] = default_color[i];
        enum_selected = default_enum;
    }
};

struct ShaderLabEffect {
    std::string name;
    std::string file_path;
    std::string engine_type;
    bool enabled = true;
    std::vector<ShaderLabUniform> uniforms;

    void reset_uniforms() {
        for (auto &u : uniforms) {
            u.reset_to_default();
        }
    }
};

class CustomUI {
public:
    CustomUI();
    ~CustomUI();

    bool initialize(HWND hwnd, ID3D11Device *device, ID3D11DeviceContext *context);
    void shutdown();

    static bool input_hook(void *user_data, UINT msg, WPARAM wParam, LPARAM lParam);

    void begin_frame();
    void render(uint32_t screen_w, uint32_t screen_h,
                uint32_t img_w, uint32_t img_h,
                float zoom_level, float pan_x, float pan_y);
    void end_frame();

    bool is_initialized() const { return m_initialized; }
    bool want_capture_mouse() const;
    bool want_capture_keyboard() const;

    float get_left_panel_width(uint32_t screen_w) const;
    float get_right_panel_width(uint32_t screen_w) const;
    float get_menu_bar_height() const;

    std::vector<ShaderLabEffect> &get_effects() { return m_effects; }
    const std::vector<ShaderLabEffect> &get_effects() const { return m_effects; }

private:
    void apply_photoshop_gray_theme();
    void render_menu_bar();
    void render_left_panel(float left_width, float top_offset, float panel_height);
    void render_center_canvas(float left_offset, float top_offset, float canvas_width, float canvas_height,
                              uint32_t img_w, uint32_t img_h,
                              float zoom_level, float pan_x, float pan_y);
    void render_right_panel(float right_offset, float top_offset, float panel_width, float panel_height);
    void add_sample_effects();

    HWND m_hwnd = nullptr;
    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_context = nullptr;
    bool m_initialized = false;

    std::vector<ShaderLabEffect> m_effects;
    int m_selected_effect_idx = 0;
    char m_search_buffer[128] = {};
    bool m_show_about_dialog = false;
};
