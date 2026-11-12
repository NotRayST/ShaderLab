#include "layer_system.h"
#include <stb_image.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

namespace {

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

struct alignas(16) LayerBlendCBData {
    float doc_dims[2];
    float layer_dims[2];
    float layer_offset[2];
    float layer_scale[2];
    float cos_rot;
    float sin_rot;
    float opacity;
    int blend_mode;
};

static const char g_layer_blend_hlsl[] = R"(
Texture2D g_BackdropTex : register(t0);
Texture2D g_SourceTex   : register(t1);
SamplerState g_LinearSampler : register(s0);

cbuffer LayerBlendCB : register(b0) {
    float2 g_DocDims;
    float2 g_LayerDims;
    float2 g_LayerOffset;
    float2 g_LayerScale;
    float  g_CosRot;
    float  g_SinRot;
    float  g_Opacity;
    int    g_BlendMode;
};

float soft_light_ch(float cb, float cs) {
    if (cs <= 0.5f) {
        return cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb);
    } else {
        float d = (cb <= 0.25f) ? (((16.0f * cb - 12.0f) * cb + 4.0f) * cb) : sqrt(cb);
        return cb + (2.0f * cs - 1.0f) * (d - cb);
    }
}

float color_dodge_ch(float cb, float cs) {
    if (cb <= 0.0f) return 0.0f;
    if (cs >= 1.0f) return 1.0f;
    return min(1.0f, cb / (1.0f - cs));
}

float color_burn_ch(float cb, float cs) {
    if (cb >= 1.0f) return 1.0f;
    if (cs <= 0.0f) return 0.0f;
    return 1.0f - min(1.0f, (1.0f - cb) / cs);
}

float3 apply_blend_mode(float3 cb, float3 cs, int mode) {
    switch (mode) {
    case 0:  return cs; // Normal
    case 1:  return cs; // Dissolve (dithered in alpha pass)
    case 2:  return min(cb, cs); // Darken
    case 3:  return cb * cs; // Multiply
    case 4:  return float3(color_burn_ch(cb.r, cs.r), color_burn_ch(cb.g, cs.g), color_burn_ch(cb.b, cs.b)); // Color Burn
    case 5:  return max(cb + cs - 1.0f, 0.0f); // Linear Burn
    case 6:  return (dot(cs, float3(0.299f, 0.587f, 0.114f)) < dot(cb, float3(0.299f, 0.587f, 0.114f))) ? cs : cb; // Darker Color
    case 7:  return max(cb, cs); // Lighten
    case 8:  return cb + cs - (cb * cs); // Screen
    case 9:  return float3(color_dodge_ch(cb.r, cs.r), color_dodge_ch(cb.g, cs.g), color_dodge_ch(cb.b, cs.b)); // Color Dodge
    case 10: return min(cb + cs, 1.0f); // Linear Dodge (Add)
    case 11: return (dot(cs, float3(0.299f, 0.587f, 0.114f)) > dot(cb, float3(0.299f, 0.587f, 0.114f))) ? cs : cb; // Lighter Color
    case 12: return float3(
                 cb.r < 0.5f ? (2.0f * cb.r * cs.r) : (1.0f - 2.0f * (1.0f - cb.r) * (1.0f - cs.r)),
                 cb.g < 0.5f ? (2.0f * cb.g * cs.g) : (1.0f - 2.0f * (1.0f - cb.g) * (1.0f - cs.g)),
                 cb.b < 0.5f ? (2.0f * cb.b * cs.b) : (1.0f - 2.0f * (1.0f - cb.b) * (1.0f - cs.b))); // Overlay
    case 13: return float3(soft_light_ch(cb.r, cs.r), soft_light_ch(cb.g, cs.g), soft_light_ch(cb.b, cs.b)); // Soft Light
    case 14: return float3(
                 cs.r < 0.5f ? (2.0f * cb.r * cs.r) : (1.0f - 2.0f * (1.0f - cb.r) * (1.0f - cs.r)),
                 cs.g < 0.5f ? (2.0f * cb.g * cs.g) : (1.0f - 2.0f * (1.0f - cb.g) * (1.0f - cs.g)),
                 cs.b < 0.5f ? (2.0f * cb.b * cs.b) : (1.0f - 2.0f * (1.0f - cb.b) * (1.0f - cs.b))); // Hard Light
    case 15: return float3(
                 cs.r < 0.5f ? color_burn_ch(cb.r, 2.0f * cs.r) : color_dodge_ch(cb.r, 2.0f * (cs.r - 0.5f)),
                 cs.g < 0.5f ? color_burn_ch(cb.g, 2.0f * cs.g) : color_dodge_ch(cb.g, 2.0f * (cs.g - 0.5f)),
                 cs.b < 0.5f ? color_burn_ch(cb.b, 2.0f * cs.b) : color_dodge_ch(cb.b, 2.0f * (cs.b - 0.5f))); // Vivid Light
    case 16: return clamp(cb + 2.0f * cs - 1.0f, 0.0f, 1.0f); // Linear Light
    case 17: return float3(
                 cs.r < 0.5f ? min(cb.r, 2.0f * cs.r) : max(cb.r, 2.0f * (cs.r - 0.5f)),
                 cs.g < 0.5f ? min(cb.g, 2.0f * cs.g) : max(cb.g, 2.0f * (cs.g - 0.5f)),
                 cs.b < 0.5f ? min(cb.b, 2.0f * cs.b) : max(cb.b, 2.0f * (cs.b - 0.5f))); // Pin Light
    case 18: return step(1.0f, cb + cs); // Hard Mix
    case 19: return abs(cb - cs); // Difference
    case 20: return cb + cs - 2.0f * cb * cs; // Exclusion
    case 21: return max(cb - cs, 0.0f); // Subtract
    case 22: return clamp(cb / max(cs, 0.0001f), 0.0f, 1.0f); // Divide
    default: return cs;
    }
}

struct VSOutput {
    float4 Pos : SV_Position;
    float2 UV  : TEXCOORD0;
};

VSOutput layer_blend_vs(uint vertex_id : SV_VertexID) {
    VSOutput output;
    output.UV = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.Pos = float4(output.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 layer_blend_ps(VSOutput input) : SV_Target {
    float4 backdrop = g_BackdropTex.Sample(g_LinearSampler, input.UV);
    float3 cb = backdrop.rgb;
    float  ab = backdrop.a;

    if (g_LayerDims.x <= 0.0f || g_LayerDims.y <= 0.0f) {
        return backdrop;
    }

    float2 doc_px = input.UV * g_DocDims;
    float2 layer_center = g_LayerDims * 0.5f + g_LayerOffset;
    float2 rel = doc_px - layer_center;
    float2 rot = float2(rel.x * g_CosRot + rel.y * g_SinRot, -rel.x * g_SinRot + rel.y * g_CosRot);
    float2 safe_scale = float2(
        abs(g_LayerScale.x) > 0.001f ? g_LayerScale.x : (g_LayerScale.x < 0.0f ? -0.001f : 0.001f),
        abs(g_LayerScale.y) > 0.001f ? g_LayerScale.y : (g_LayerScale.y < 0.0f ? -0.001f : 0.001f)
    );
    float2 layer_px = (rot / safe_scale) + (g_LayerDims * 0.5f);
    float2 layer_uv = layer_px / g_LayerDims;

    if (layer_uv.x < 0.0f || layer_uv.x > 1.0f || layer_uv.y < 0.0f || layer_uv.y > 1.0f) {
        return backdrop;
    }

    float4 src = g_SourceTex.Sample(g_LinearSampler, layer_uv);
    float3 cs = src.rgb;
    float  as_val = saturate(src.a * g_Opacity);

    // Stochastic dithering for Photoshop Dissolve mode
    if (g_BlendMode == 1) {
        uint2 px_i = uint2(floor(doc_px));
        uint h = (px_i.x * 374761393u) ^ (px_i.y * 668265263u) ^ 0x9e3779b9u;
        h = (h ^ (h >> 13u)) * 1274126177u;
        float dither = float((h ^ (h >> 16u)) & 0xFFFFu) / 65536.0f;
        as_val = (as_val > dither) ? 1.0f : 0.0f;
    }

    if (as_val <= 0.0001f) {
        return backdrop;
    }

    float3 blended = apply_blend_mode(cb, cs, g_BlendMode);
    float  ao = as_val + ab * (1.0f - as_val);
    float3 co_premul = as_val * (1.0f - ab) * cs + as_val * ab * blended + (1.0f - as_val) * ab * cb;
    float3 co = (ao > 0.0001f) ? saturate(co_premul / ao) : float3(0.0f, 0.0f, 0.0f);
    return float4(co, ao);
}
)";

bool read_file_bytes(const std::wstring &path, std::vector<uint8_t> &out_bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER file_size = {};
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0 || file_size.QuadPart > 512LL * 1024LL * 1024LL) {
        CloseHandle(file);
        return false;
    }

    out_bytes.resize(static_cast<size_t>(file_size.QuadPart));
    DWORD bytes_read = 0;
    BOOL ok = ReadFile(file, out_bytes.data(), static_cast<DWORD>(out_bytes.size()), &bytes_read, nullptr);
    CloseHandle(file);
    if (!ok || bytes_read != out_bytes.size()) {
        out_bytes.clear();
        return false;
    }
    return true;
}

} // namespace

const char *blend_mode_name(gui::BlendMode mode) {
    switch (mode) {
    case gui::BlendMode::Normal:      return "Normal";
    case gui::BlendMode::Dissolve:    return "Dissolve";
    case gui::BlendMode::Darken:      return "Darken";
    case gui::BlendMode::Multiply:    return "Multiply";
    case gui::BlendMode::ColorBurn:   return "Color Burn";
    case gui::BlendMode::LinearBurn:  return "Linear Burn";
    case gui::BlendMode::DarkerColor: return "Darker Color";
    case gui::BlendMode::Lighten:     return "Lighten";
    case gui::BlendMode::Screen:      return "Screen";
    case gui::BlendMode::ColorDodge:  return "Color Dodge";
    case gui::BlendMode::LinearDodge: return "Linear Dodge (Add)";
    case gui::BlendMode::LighterColor:return "Lighter Color";
    case gui::BlendMode::Overlay:     return "Overlay";
    case gui::BlendMode::SoftLight:   return "Soft Light";
    case gui::BlendMode::HardLight:   return "Hard Light";
    case gui::BlendMode::VividLight:  return "Vivid Light";
    case gui::BlendMode::LinearLight: return "Linear Light";
    case gui::BlendMode::PinLight:    return "Pin Light";
    case gui::BlendMode::HardMix:     return "Hard Mix";
    case gui::BlendMode::Difference:  return "Difference";
    case gui::BlendMode::Exclusion:   return "Exclusion";
    case gui::BlendMode::Subtract:    return "Subtract";
    case gui::BlendMode::Divide:      return "Divide";
    default:                          return "Normal";
    }
}

void DocumentLayer::sync_to_gpu(ID3D11Device *device, ID3D11DeviceContext *context) {
    if (!device) return;
    if (width == 0 || height == 0 || rgba_pixels.size() != static_cast<size_t>(width) * height * 4) {
        srv.Reset();
        texture.Reset();
        gpu_dirty = false;
        return;
    }

    bool need_recreate = true;
    if (texture && srv) {
        D3D11_TEXTURE2D_DESC existing_desc = {};
        texture->GetDesc(&existing_desc);
        if (existing_desc.Width == width && existing_desc.Height == height &&
            existing_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
            existing_desc.Usage == D3D11_USAGE_DEFAULT) {
            need_recreate = false;
        }
    }

    if (!need_recreate) {
        ComPtr<ID3D11DeviceContext> imm_ctx;
        ID3D11DeviceContext *ctx = context;
        if (!ctx) {
            device->GetImmediateContext(&imm_ctx);
            ctx = imm_ctx.Get();
        }
        if (ctx) {
            ctx->UpdateSubresource(texture.Get(), 0, nullptr, rgba_pixels.data(), width * 4, 0);
            gpu_dirty = false;
            return;
        }
    }

    srv.Reset();
    texture.Reset();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init_data = {};
    init_data.pSysMem = rgba_pixels.data();
    init_data.SysMemPitch = width * 4;

    if (FAILED(device->CreateTexture2D(&desc, &init_data, &texture))) {
        return;
    }
    if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
        texture.Reset();
        return;
    }
    gpu_dirty = false;
}

DocumentLayer *LayerStack::active_layer() {
    if (active_index < 0 || active_index >= static_cast<int>(layers.size())) {
        return nullptr;
    }
    return &layers[static_cast<size_t>(active_index)];
}

const DocumentLayer *LayerStack::active_layer() const {
    if (active_index < 0 || active_index >= static_cast<int>(layers.size())) {
        return nullptr;
    }
    return &layers[static_cast<size_t>(active_index)];
}

void LayerStack::clear() {
    layers.clear();
    active_index = -1;
    doc_width = 0;
    doc_height = 0;
    next_layer_id = 1;
}

int LayerStack::add_empty_layer(const std::string &name, ID3D11Device *device) {
    uint32_t w = (doc_width > 0) ? doc_width : 512;
    uint32_t h = (doc_height > 0) ? doc_height : 512;
    if (doc_width == 0) doc_width = w;
    if (doc_height == 0) doc_height = h;

    DocumentLayer layer;
    layer.id = next_layer_id++;
    layer.name = name.empty() ? ("Layer " + std::to_string(layer.id)) : name;
    layer.width = w;
    layer.height = h;
    layer.rgba_pixels.assign(static_cast<size_t>(w) * h * 4, 0);
    layer.gpu_dirty = true;
    if (device) {
        layer.sync_to_gpu(device);
    }

    int insert_pos = (active_index >= 0 && active_index < static_cast<int>(layers.size()))
        ? (active_index + 1)
        : static_cast<int>(layers.size());
    layers.insert(layers.begin() + insert_pos, std::move(layer));
    active_index = insert_pos;
    return active_index;
}

int LayerStack::add_layer_from_rgba(
    const std::string &name,
    uint32_t w,
    uint32_t h,
    const uint8_t *rgba_data,
    size_t byte_count,
    ID3D11Device *device,
    bool make_active
) {
    if (w == 0 || h == 0 || !rgba_data) return -1;
    size_t expected = static_cast<size_t>(w) * h * 4;
    if (byte_count < expected) return -1;

    if (doc_width == 0 || doc_height == 0) {
        doc_width = w;
        doc_height = h;
    }

    DocumentLayer layer;
    layer.id = next_layer_id++;
    layer.name = name.empty() ? ("Layer " + std::to_string(layer.id)) : name;
    layer.width = w;
    layer.height = h;
    layer.rgba_pixels.assign(rgba_data, rgba_data + expected);
    layer.gpu_dirty = true;
    if (device) {
        layer.sync_to_gpu(device);
    }

    int insert_pos = (active_index >= 0 && active_index < static_cast<int>(layers.size()))
        ? (active_index + 1)
        : static_cast<int>(layers.size());
    layers.insert(layers.begin() + insert_pos, std::move(layer));
    if (make_active || active_index < 0) {
        active_index = insert_pos;
    }
    return insert_pos;
}

int LayerStack::import_image_layer(const std::wstring &file_path, ID3D11Device *device, std::wstring *out_error) {
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(file_path, bytes)) {
        if (out_error) *out_error = L"Failed to open image file.";
        return -1;
    }

    int w = 0, h = 0, channels = 0;
    stbi_uc *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        if (out_error) *out_error = L"Unsupported or corrupt image format.";
        return -1;
    }

    std::string layer_name;
    size_t slash = file_path.find_last_of(L"\\/");
    std::wstring fname = (slash == std::wstring::npos) ? file_path : file_path.substr(slash + 1);
    layer_name = gui::to_utf8(fname);

    size_t byte_count = static_cast<size_t>(w) * h * 4;
    int idx = add_layer_from_rgba(layer_name, static_cast<uint32_t>(w), static_cast<uint32_t>(h), pixels, byte_count, device, true);
    stbi_image_free(pixels);
    return idx;
}

int LayerStack::duplicate_active_layer(ID3D11Device *device) {
    const DocumentLayer *src = active_layer();
    if (!src) return -1;

    DocumentLayer copy = *src;
    copy.id = next_layer_id++;
    copy.name = src->name + " Copy";
    copy.reset_gpu();
    if (device) {
        copy.sync_to_gpu(device);
    }

    int insert_pos = active_index + 1;
    layers.insert(layers.begin() + insert_pos, std::move(copy));
    active_index = insert_pos;
    return active_index;
}

bool LayerStack::merge_down(ID3D11Device *device, ID3D11DeviceContext *context, LayerCompositor &compositor) {
    if (!device || !context) return false;
    if (active_index <= 0 || active_index >= static_cast<int>(layers.size())) {
        return false;
    }

    DocumentLayer &lower = layers[static_cast<size_t>(active_index - 1)];
    DocumentLayer &upper = layers[static_cast<size_t>(active_index)];
    if (lower.lock_pixels) {
        return false;
    }

    uint32_t w = (doc_width > 0) ? doc_width : (std::max)(lower.width, upper.width);
    uint32_t h = (doc_height > 0) ? doc_height : (std::max)(lower.height, upper.height);
    if (w == 0 || h == 0) return false;

    if (!compositor.composite_pair(context, w, h, lower, upper)) {
        return false;
    }

    uint32_t out_w = 0, out_h = 0;
    std::vector<uint8_t> merged_rgba;
    if (!LayerCompositor::readback_texture_rgba(device, context, compositor.get_composited_texture(), out_w, out_h, merged_rgba)) {
        return false;
    }

    lower.width = out_w;
    lower.height = out_h;
    lower.rgba_pixels = std::move(merged_rgba);
    lower.visible = true;
    lower.blend_mode = gui::BlendMode::Normal;
    lower.opacity = 1.0f;
    lower.fill_opacity = 1.0f;
    lower.offset_x = 0.0f;
    lower.offset_y = 0.0f;
    lower.rotation_deg = 0.0f;
    lower.scale_x = 1.0f;
    lower.scale_y = 1.0f;
    lower.is_text_layer = false;
    lower.mark_dirty();
    lower.sync_to_gpu(device, context);

    layers.erase(layers.begin() + active_index);
    active_index = active_index - 1;
    return true;
}

bool LayerStack::delete_active_layer(ID3D11Device *device) {
    if (active_index < 0 || active_index >= static_cast<int>(layers.size())) {
        return false;
    }

    if (layers.size() <= 1) {
        DocumentLayer &only = layers[0];
        if (only.lock_pixels) return false;
        uint32_t w = (doc_width > 0) ? doc_width : only.width;
        uint32_t h = (doc_height > 0) ? doc_height : only.height;
        only.name = "Layer 1";
        only.visible = true;
        only.blend_mode = gui::BlendMode::Normal;
        only.opacity = 1.0f;
        only.fill_opacity = 1.0f;
        only.offset_x = 0.0f;
        only.offset_y = 0.0f;
        only.rotation_deg = 0.0f;
        only.scale_x = 1.0f;
        only.scale_y = 1.0f;
        only.is_text_layer = false;
        only.is_adjustment_layer = false;
        only.text_content.clear();
        only.width = w;
        only.height = h;
        only.rgba_pixels.assign(static_cast<size_t>(w) * h * 4, 0);
        only.mark_dirty();
        if (device) {
            only.sync_to_gpu(device);
        }
        active_index = 0;
        return true;
    }

    layers.erase(layers.begin() + active_index);
    if (active_index >= static_cast<int>(layers.size())) {
        active_index = static_cast<int>(layers.size()) - 1;
    }
    return true;
}

bool LayerStack::move_layer_up(int index) {
    return reorder_layer(index, index + 1);
}

bool LayerStack::move_layer_down(int index) {
    return reorder_layer(index, index - 1);
}

bool LayerStack::reorder_layer(int from_index, int to_index) {
    int count = static_cast<int>(layers.size());
    if (from_index < 0 || from_index >= count || to_index < 0 || to_index >= count || from_index == to_index) {
        return false;
    }

    uint32_t active_id = (active_layer() != nullptr) ? active_layer()->id : 0;

    DocumentLayer moved = std::move(layers[static_cast<size_t>(from_index)]);
    layers.erase(layers.begin() + from_index);
    layers.insert(layers.begin() + to_index, std::move(moved));

    if (active_id != 0) {
        for (int i = 0; i < count; ++i) {
            if (layers[static_cast<size_t>(i)].id == active_id) {
                active_index = i;
                break;
            }
        }
    }
    return true;
}

bool LayerStack::crop_all_layers(uint32_t crop_x, uint32_t crop_y, uint32_t crop_w, uint32_t crop_h, ID3D11Device *device) {
    if (crop_w == 0 || crop_h == 0) return false;

    for (auto &layer : layers) {
        if (layer.width == 0 || layer.height == 0 || layer.rgba_pixels.size() != static_cast<size_t>(layer.width) * layer.height * 4) {
            continue;
        }
        std::vector<uint8_t> cropped(static_cast<size_t>(crop_w) * crop_h * 4, 0);
        int off_x = static_cast<int>(std::lround(layer.offset_x));
        int off_y = static_cast<int>(std::lround(layer.offset_y));

        for (uint32_t dy = 0; dy < crop_h; ++dy) {
            int sx_base = static_cast<int>(crop_x) - off_x;
            int sy = static_cast<int>(crop_y + dy) - off_y;
            if (sy < 0 || sy >= static_cast<int>(layer.height)) continue;

            for (uint32_t dx = 0; dx < crop_w; ++dx) {
                int sx = sx_base + static_cast<int>(dx);
                if (sx < 0 || sx >= static_cast<int>(layer.width)) continue;
                size_t src_idx = (static_cast<size_t>(sy) * layer.width + static_cast<size_t>(sx)) * 4;
                size_t dst_idx = (static_cast<size_t>(dy) * crop_w + static_cast<size_t>(dx)) * 4;
                std::memcpy(&cropped[dst_idx], &layer.rgba_pixels[src_idx], 4);
            }
        }

        layer.width = crop_w;
        layer.height = crop_h;
        layer.offset_x = 0.0f;
        layer.offset_y = 0.0f;
        layer.rgba_pixels = std::move(cropped);
        layer.mark_dirty();
        if (device) {
            layer.sync_to_gpu(device);
        }
    }

    doc_width = crop_w;
    doc_height = crop_h;
    return true;
}

bool LayerStack::commit_active_pixels(ID3D11Device *device, ID3D11DeviceContext *context) {
    DocumentLayer *layer = active_layer();
    if (!layer || !device) return false;
    layer->mark_dirty();
    layer->sync_to_gpu(device, context);
    return (layer->srv != nullptr);
}

bool LayerCompositor::initialize(ID3D11Device *device) {
    if (!device) return false;
    m_device = device;

    ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;

    HRESULT hr = D3DCompile(g_layer_blend_hlsl, std::strlen(g_layer_blend_hlsl), "layer_blend_vs",
                            nullptr, nullptr, "layer_blend_vs", "vs_5_0", flags, 0, &vs_blob, &err_blob);
    if (FAILED(hr)) {
        if (err_blob) {
            std::cerr << "[LayerCompositor] VS compile failed: "
                      << static_cast<const char *>(err_blob->GetBufferPointer()) << "\n";
        }
        return false;
    }

    hr = device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vs);
    if (FAILED(hr)) return false;

    err_blob.Reset();
    hr = D3DCompile(g_layer_blend_hlsl, std::strlen(g_layer_blend_hlsl), "layer_blend_ps",
                    nullptr, nullptr, "layer_blend_ps", "ps_5_0", flags, 0, &ps_blob, &err_blob);
    if (FAILED(hr)) {
        if (err_blob) {
            std::cerr << "[LayerCompositor] PS compile failed: "
                      << static_cast<const char *>(err_blob->GetBufferPointer()) << "\n";
        }
        return false;
    }

    hr = device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_ps_blend);
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC samp_desc = {};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp_desc.MaxLOD = D3D11_FLOAT32_MAX;

    if (FAILED(device->CreateSamplerState(&samp_desc, &m_sampler_linear))) {
        return false;
    }

    D3D11_BUFFER_DESC cb_desc = {};
    cb_desc.ByteWidth = sizeof(LayerBlendCBData);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    return SUCCEEDED(device->CreateBuffer(&cb_desc, nullptr, &m_cbuffer));
}

void LayerCompositor::shutdown() {
    m_last_tex = nullptr;
    m_last_srv = nullptr;
    m_comp_srv_a.Reset();
    m_comp_srv_b.Reset();
    m_comp_rtv_a.Reset();
    m_comp_rtv_b.Reset();
    m_comp_tex_a.Reset();
    m_comp_tex_b.Reset();
    m_w = 0;
    m_h = 0;
    m_cbuffer.Reset();
    m_sampler_linear.Reset();
    m_ps_blend.Reset();
    m_vs.Reset();
    m_device.Reset();
}

bool LayerCompositor::ensure_targets(uint32_t w, uint32_t h) {
    if (!m_device || w == 0 || h == 0) return false;
    if (m_w == w && m_h == h && m_comp_tex_a && m_comp_tex_b && m_comp_rtv_a && m_comp_rtv_b && m_comp_srv_a && m_comp_srv_b) {
        return true;
    }

    m_comp_srv_a.Reset();
    m_comp_srv_b.Reset();
    m_comp_rtv_a.Reset();
    m_comp_rtv_b.Reset();
    m_comp_tex_a.Reset();
    m_comp_tex_b.Reset();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_comp_tex_a))) return false;
    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_comp_tex_b))) return false;
    if (FAILED(m_device->CreateRenderTargetView(m_comp_tex_a.Get(), nullptr, &m_comp_rtv_a))) return false;
    if (FAILED(m_device->CreateRenderTargetView(m_comp_tex_b.Get(), nullptr, &m_comp_rtv_b))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_comp_tex_a.Get(), nullptr, &m_comp_srv_a))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_comp_tex_b.Get(), nullptr, &m_comp_srv_b))) return false;

    m_w = w;
    m_h = h;
    return true;
}

ID3D11ShaderResourceView *LayerCompositor::composite_slice(
    ID3D11DeviceContext *context,
    uint32_t doc_w,
    uint32_t doc_h,
    DocumentLayer *const *layer_ptrs,
    size_t count
) {
    if (!context || !m_device || !m_vs || !m_ps_blend || !m_cbuffer) return nullptr;
    if (!ensure_targets(doc_w, doc_h)) return nullptr;

    ID3D11ShaderResourceView *null_srvs[2] = { nullptr, nullptr };
    context->PSSetShaderResources(0, 2, null_srvs);

    const float clear_zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->ClearRenderTargetView(m_comp_rtv_a.Get(), clear_zero);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(doc_w);
    vp.Height = static_cast<float>(doc_h);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    float blend_factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState(nullptr, blend_factor, 0xFFFFFFFF);
    context->OMSetDepthStencilState(nullptr, 0);
    context->RSSetState(nullptr);
    context->RSSetViewports(1, &vp);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps_blend.Get(), nullptr, 0);

    ID3D11SamplerState *samplers[] = { m_sampler_linear.Get() };
    context->PSSetSamplers(0, 1, samplers);

    bool src_is_a = true;

    for (size_t i = 0; i < count; ++i) {
        DocumentLayer *layer = layer_ptrs[i];
        if (!layer || !layer->visible) continue;

        if (layer->gpu_dirty || !layer->srv) {
            layer->sync_to_gpu(m_device.Get(), context);
        }
        if (!layer->srv || layer->width == 0 || layer->height == 0) continue;

        float eff_opacity = std::clamp(layer->opacity, 0.0f, 1.0f) * std::clamp(layer->fill_opacity, 0.0f, 1.0f);
        if (eff_opacity <= 0.0001f) continue;

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context->Map(m_cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto *cb = static_cast<LayerBlendCBData *>(mapped.pData);
            float rad = layer->rotation_deg * kDegToRad;
            cb->doc_dims[0] = static_cast<float>(doc_w);
            cb->doc_dims[1] = static_cast<float>(doc_h);
            cb->layer_dims[0] = static_cast<float>(layer->width);
            cb->layer_dims[1] = static_cast<float>(layer->height);
            cb->layer_offset[0] = layer->offset_x;
            cb->layer_offset[1] = layer->offset_y;
            cb->layer_scale[0] = layer->scale_x;
            cb->layer_scale[1] = layer->scale_y;
            cb->cos_rot = std::cos(rad);
            cb->sin_rot = std::sin(rad);
            cb->opacity = eff_opacity;
            cb->blend_mode = static_cast<int>(layer->blend_mode);
            context->Unmap(m_cbuffer.Get(), 0);
        }

        ID3D11ShaderResourceView *backdrop_srv = src_is_a ? m_comp_srv_a.Get() : m_comp_srv_b.Get();
        ID3D11RenderTargetView *dst_rtv = src_is_a ? m_comp_rtv_b.Get() : m_comp_rtv_a.Get();

        context->PSSetShaderResources(0, 2, null_srvs);
        context->OMSetRenderTargets(1, &dst_rtv, nullptr);

        ID3D11Buffer *cbs[] = { m_cbuffer.Get() };
        context->PSSetConstantBuffers(0, 1, cbs);

        ID3D11ShaderResourceView *srvs[2] = { backdrop_srv, layer->srv.Get() };
        context->PSSetShaderResources(0, 2, srvs);

        context->Draw(3, 0);
        src_is_a = !src_is_a;
    }

    context->PSSetShaderResources(0, 2, null_srvs);
    ID3D11RenderTargetView *null_rtv = nullptr;
    context->OMSetRenderTargets(1, &null_rtv, nullptr);

    m_last_tex = src_is_a ? m_comp_tex_a.Get() : m_comp_tex_b.Get();
    m_last_srv = src_is_a ? m_comp_srv_a.Get() : m_comp_srv_b.Get();
    return m_last_srv;
}

ID3D11ShaderResourceView *LayerCompositor::composite_stack(
    ID3D11DeviceContext *context,
    uint32_t doc_w,
    uint32_t doc_h,
    std::vector<DocumentLayer> &layers
) {
    std::vector<DocumentLayer *> ptrs;
    ptrs.reserve(layers.size());
    for (auto &layer : layers) {
        ptrs.push_back(&layer);
    }
    return composite_slice(context, doc_w, doc_h, ptrs.data(), ptrs.size());
}

ID3D11ShaderResourceView *LayerCompositor::composite_pair(
    ID3D11DeviceContext *context,
    uint32_t doc_w,
    uint32_t doc_h,
    DocumentLayer &bottom,
    DocumentLayer &top
) {
    DocumentLayer *ptrs[2] = { &bottom, &top };
    return composite_slice(context, doc_w, doc_h, ptrs, 2);
}

bool LayerCompositor::readback_texture_rgba(
    ID3D11Device *device,
    ID3D11DeviceContext *context,
    ID3D11Texture2D *tex,
    uint32_t &out_w,
    uint32_t &out_h,
    std::vector<uint8_t> &out_rgba
) {
    if (!device || !context || !tex) return false;

    D3D11_TEXTURE2D_DESC src_desc = {};
    tex->GetDesc(&src_desc);
    if (src_desc.Width == 0 || src_desc.Height == 0) return false;

    D3D11_TEXTURE2D_DESC staging_desc = {};
    staging_desc.Width = src_desc.Width;
    staging_desc.Height = src_desc.Height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = src_desc.Format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
        return false;
    }

    context->CopyResource(staging.Get(), tex);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    bool is_bgra = (src_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || src_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    out_w = src_desc.Width;
    out_h = src_desc.Height;
    out_rgba.resize(static_cast<size_t>(out_w) * out_h * 4);

    const uint8_t *src_bytes = static_cast<const uint8_t *>(mapped.pData);
    for (uint32_t y = 0; y < out_h; ++y) {
        const uint8_t *row_src = src_bytes + static_cast<size_t>(y) * mapped.RowPitch;
        uint8_t *row_dst = out_rgba.data() + static_cast<size_t>(y) * out_w * 4;
        if (!is_bgra) {
            std::memcpy(row_dst, row_src, static_cast<size_t>(out_w) * 4);
        } else {
            for (uint32_t x = 0; x < out_w; ++x) {
                row_dst[x * 4 + 0] = row_src[x * 4 + 2];
                row_dst[x * 4 + 1] = row_src[x * 4 + 1];
                row_dst[x * 4 + 2] = row_src[x * 4 + 0];
                row_dst[x * 4 + 3] = row_src[x * 4 + 3];
            }
        }
    }

    context->Unmap(staging.Get(), 0);
    return true;
}
