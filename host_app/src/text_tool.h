#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d2d1.h>
#include <dwrite_1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

namespace gui {
class GuiManager;
}

struct DocumentLayer;

namespace shaderlab {

using Microsoft::WRL::ComPtr;

struct SystemFontEntry {
    std::wstring family_w;
    std::string family_utf8;
};

struct TextToolSettings {
    std::string text_utf8 = "ShaderLab Text";
    std::wstring font_family = L"Segoe UI";
    float font_size = 48.0f;
    bool font_bold = false;
    bool font_italic = false;
    bool font_underline = false;
    bool font_strikethrough = false;
    int text_align = 0;       // 0=Left, 1=Center, 2=Right, 3=Justify
    float line_spacing = 1.15f;
    float tracking = 0.0f;    // -50..+200 (1/1000 em units)
    float text_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float stroke_width = 0.0f;
    float stroke_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    bool shadow_enabled = false;
    float shadow_offset_x = 2.0f;
    float shadow_offset_y = 2.0f;
    float shadow_blur = 3.0f;
    float shadow_color[4] = { 0.0f, 0.0f, 0.0f, 0.6f };
};

class TextToolEngine {
public:
    static TextToolEngine &get();

    TextToolEngine(const TextToolEngine &) = delete;
    TextToolEngine &operator=(const TextToolEngine &) = delete;
    TextToolEngine(TextToolEngine &&) = delete;
    TextToolEngine &operator=(TextToolEngine &&) = delete;

    bool initialize();
    void shutdown();

    bool refresh_system_fonts();
    const std::vector<SystemFontEntry> &get_system_fonts() const { return m_system_fonts; }

    // Rasterize straight RGBA8 buffer from standalone style settings
    bool rasterize_text_rgba(
        const TextToolSettings &style,
        uint32_t doc_w,
        uint32_t doc_h,
        std::vector<uint8_t> &out_rgba,
        uint32_t &out_w,
        uint32_t &out_h
    );

    // Rasterize directly into DocumentLayer and upload texture to D3D11
    bool rasterize_text_layer(
        ID3D11Device *device,
        DocumentLayer &layer,
        uint32_t doc_w,
        uint32_t doc_h,
        ID3D11DeviceContext *context = nullptr
    );

    // Copy active settings <-> DocumentLayer
    void sync_settings_from_layer(const DocumentLayer &layer);
    void apply_settings_to_layer(DocumentLayer &layer) const;

    TextToolSettings &settings_mut() { return m_settings; }
    const TextToolSettings &settings() const { return m_settings; }

    // Canvas click / create / live-update request flags consumed by main loop
    void on_canvas_click(float doc_x, float doc_y);
    bool pop_create_layer_request(float &out_doc_x, float &out_doc_y);
    bool pop_live_update_request();

    // Right-panel UI helper (registers required DOM IDs and respects AGENTS.MD layout rules)
    void render_text_tab(
        gui::GuiManager &gui,
        bool has_document,
        bool active_layer_is_text,
        bool include_apply_button = true
    );
    void render_text_bottom_button(
        gui::GuiManager &gui,
        bool has_document,
        bool active_layer_is_text
    );

private:
    TextToolEngine() = default;
    ~TextToolEngine();

    bool ensure_factories();

    ComPtr<IDWriteFactory1> m_dwrite_factory;
    ComPtr<ID2D1Factory> m_d2d_factory;
    ComPtr<IWICImagingFactory> m_wic_factory;
    std::vector<SystemFontEntry> m_system_fonts;
    bool m_com_initialized_by_us = false;
    bool m_initialized = false;

    TextToolSettings m_settings;
    char m_font_filter[128] = {};
    char m_text_edit_buf[8192] = {};
    bool m_text_buf_synced = false;

    bool m_req_create_layer = false;
    float m_req_create_x = 0.0f;
    float m_req_create_y = 0.0f;
    bool m_req_live_update = false;
};

} // namespace shaderlab
