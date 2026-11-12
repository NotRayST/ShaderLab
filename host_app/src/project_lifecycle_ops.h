#pragma once

#include "project_file.h"
#include "GUI/types.h"
#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

namespace lifecycle_ops {

// Serializes the active effect chain and uniforms to ReShade + ShaderLab INI bytes.
std::vector<uint8_t> serialize_effects_to_preset_ini(const std::vector<gui::ShaderLabEffect> &effects);

// Applies serialized INI bytes back onto an effects vector (supports both ShaderLab v2 INI and vanilla ReShade INI).
bool deserialize_effects_from_preset_ini(const std::vector<uint8_t> &ini_bytes, std::vector<gui::ShaderLabEffect> &effects);
bool load_preset_ini_file(const std::wstring &ini_path, std::vector<gui::ShaderLabEffect> &effects);

// Memory PNG encode / decode (RGBA8).
bool encode_rgba_to_png_bytes(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &out_png_bytes);
bool decode_png_bytes_to_rgba(const std::vector<uint8_t> &png_bytes, std::vector<uint8_t> &out_rgba, uint32_t &out_w, uint32_t &out_h);

// Reads back a D3D11 texture into packed RGBA8 bytes (converting BGRA -> RGBA automatically).
bool readback_texture_rgba(
    ID3D11Device *device,
    ID3D11DeviceContext *context,
    ID3D11Texture2D *tex,
    std::vector<uint8_t> &out_rgba,
    uint32_t &out_w,
    uint32_t &out_h,
    bool force_opaque_alpha = false
);

// Win32 file prompt helpers.
std::wstring ensure_project_extension(const std::wstring &path);
bool prompt_save_project_path(HWND hwnd, const std::wstring &default_stem, std::wstring &out_path);
bool prompt_export_image_path(HWND hwnd, int default_export_format, const std::wstring &default_stem, std::wstring &out_path);

// Writes packed RGBA8 pixels to .png, .jpg/.jpeg, .tga, or .bmp (Unicode-safe).
bool write_rgba_image_file(
    const std::wstring &output_path,
    const uint8_t *rgba,
    uint32_t width,
    uint32_t height,
    int jpeg_quality = 92
);

// High-level Save / Save As project handler (if cancelled by user, returns false with empty out_error).
bool execute_save_project_archive(
    HWND hwnd,
    std::wstring &in_out_project_path,
    bool force_save_as,
    const std::wstring &source_image_path,
    const std::vector<gui::ShaderLabEffect> &effects,
    const project_file::ViewportState &view_state,
    const project_file::DepthState &depth_state,
    const std::vector<project_file::SerializedLayerEntry> &layers,
    uint32_t canvas_width,
    uint32_t canvas_height,
    int active_layer_index,
    const std::wstring &default_stem,
    std::wstring &out_error
);

// High-level Export / Export As handler from rendered D3D11 texture (if cancelled, returns false with empty out_error).
bool execute_export_texture(
    HWND hwnd,
    ID3D11Device *device,
    ID3D11DeviceContext *context,
    ID3D11Texture2D *rendered_tex,
    std::wstring &in_out_export_path,
    bool force_export_as,
    int default_export_format,
    int jpeg_quality,
    const std::wstring &default_stem,
    std::wstring &out_error
);

} // namespace lifecycle_ops
