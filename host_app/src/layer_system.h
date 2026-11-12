#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstdint>
#include "GUI/types.h"

using Microsoft::WRL::ComPtr;

class LayerCompositor;

const char *blend_mode_name(gui::BlendMode mode);

struct DocumentLayer {
    uint32_t id = 0;
    std::string name = "Layer 1";
    bool visible = true;
    gui::BlendMode blend_mode = gui::BlendMode::Normal;
    float opacity = 1.0f;      // 0.0 .. 1.0
    float fill_opacity = 1.0f; // 0.0 .. 1.0

    // Intra-frame transform (modified by Move Tool V and Rotate Tool R)
    float offset_x = 0.0f;     // in document pixels
    float offset_y = 0.0f;
    float rotation_deg = 0.0f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;

    bool lock_alpha = false;
    bool lock_pixels = false;
    bool lock_position = false;
    bool is_adjustment_layer = false;

    // CPU RGBA8 master buffer (for raster eraser, crop, and serialization) + GPU texture/SRV
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba_pixels;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    bool gpu_dirty = true;

    // Text Layer metadata (Feature 15)
    bool is_text_layer = false;
    std::wstring text_content;
    std::wstring font_family = L"Segoe UI";
    float font_size = 48.0f;
    bool font_bold = false;
    bool font_italic = false;
    bool font_underline = false;
    bool font_strikethrough = false;
    int text_align = 0; // 0=Left, 1=Center, 2=Right
    float line_spacing = 1.15f;
    float tracking = 0.0f;
    float text_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float stroke_width = 0.0f;
    float stroke_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    bool shadow_enabled = false;
    float shadow_offset_x = 2.0f;
    float shadow_offset_y = 2.0f;
    float shadow_blur = 3.0f;
    float shadow_color[4] = { 0.0f, 0.0f, 0.0f, 0.6f };

    void mark_dirty() { gpu_dirty = true; }
    bool is_locked_all() const { return lock_alpha && lock_pixels && lock_position; }
    void set_locked_all(bool locked) { lock_alpha = lock_pixels = lock_position = locked; }
    void reset_gpu() { srv.Reset(); texture.Reset(); gpu_dirty = true; }
    void sync_to_gpu(ID3D11Device *device, ID3D11DeviceContext *context = nullptr);
};

struct LayerStack {
    uint32_t doc_width = 0;
    uint32_t doc_height = 0;
    std::vector<DocumentLayer> layers; // stored bottom-to-top: [0] = bottom, back() = top
    int active_index = -1;
    uint32_t next_layer_id = 1;

    DocumentLayer *active_layer();
    const DocumentLayer *active_layer() const;

    void clear();

    int add_empty_layer(const std::string &name = "", ID3D11Device *device = nullptr);
    int add_layer_from_rgba(
        const std::string &name,
        uint32_t w,
        uint32_t h,
        const uint8_t *rgba_data,
        size_t byte_count,
        ID3D11Device *device = nullptr,
        bool make_active = true
    );
    int import_image_layer(const std::wstring &file_path, ID3D11Device *device, std::wstring *out_error = nullptr);
    int duplicate_active_layer(ID3D11Device *device = nullptr);
    bool merge_down(ID3D11Device *device, ID3D11DeviceContext *context, LayerCompositor &compositor);
    bool delete_active_layer(ID3D11Device *device = nullptr);
    bool move_layer_up(int index);
    bool move_layer_down(int index);
    bool reorder_layer(int from_index, int to_index);
    bool crop_all_layers(uint32_t crop_x, uint32_t crop_y, uint32_t crop_w, uint32_t crop_h, ID3D11Device *device = nullptr);
    bool commit_active_pixels(ID3D11Device *device, ID3D11DeviceContext *context = nullptr);
};

class LayerCompositor {
public:
    bool initialize(ID3D11Device *device);
    void shutdown();

    // Composites all visible layers bottom-to-top into an unpremultiplied RGBA8 document texture
    // preserving true alpha (0.0 where transparent) for downstream shaders and export.
    ID3D11ShaderResourceView *composite_stack(
        ID3D11DeviceContext *context,
        uint32_t doc_w,
        uint32_t doc_h,
        std::vector<DocumentLayer> &layers
    );

    // Composites a two-layer slice into a doc_w x doc_h RGBA8 target (used by Merge Down).
    ID3D11ShaderResourceView *composite_pair(
        ID3D11DeviceContext *context,
        uint32_t doc_w,
        uint32_t doc_h,
        DocumentLayer &bottom,
        DocumentLayer &top
    );

    ID3D11Texture2D *get_composited_texture() const { return m_last_tex; }
    ID3D11ShaderResourceView *get_composited_srv() const { return m_last_srv; }

    static bool readback_texture_rgba(
        ID3D11Device *device,
        ID3D11DeviceContext *context,
        ID3D11Texture2D *tex,
        uint32_t &out_w,
        uint32_t &out_h,
        std::vector<uint8_t> &out_rgba
    );

private:
    bool ensure_targets(uint32_t w, uint32_t h);
    ID3D11ShaderResourceView *composite_slice(
        ID3D11DeviceContext *context,
        uint32_t doc_w,
        uint32_t doc_h,
        DocumentLayer *const *layer_ptrs,
        size_t count
    );

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps_blend;
    ComPtr<ID3D11SamplerState> m_sampler_linear;
    ComPtr<ID3D11Buffer> m_cbuffer;

    uint32_t m_w = 0;
    uint32_t m_h = 0;
    ComPtr<ID3D11Texture2D> m_comp_tex_a, m_comp_tex_b;
    ComPtr<ID3D11RenderTargetView> m_comp_rtv_a, m_comp_rtv_b;
    ComPtr<ID3D11ShaderResourceView> m_comp_srv_a, m_comp_srv_b;
    ID3D11Texture2D *m_last_tex = nullptr;
    ID3D11ShaderResourceView *m_last_srv = nullptr;
};
