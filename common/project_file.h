#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace project_file {

struct ViewportState {
    float zoom = 1.0f;
    float angle = 0.0f;
    float pan[2] = { 0.0f, 0.0f };
    bool is_fullscreen = false;
    bool lock_pan = false;
    bool lock_zoom = false;
    bool lock_rot = false;
};

struct DepthState {
    bool has_depth = false;
    float far_plane = 1000.0f;
    std::wstring depth_sidecar_path;
};

struct ProjectManifest {
    std::string format_version = "1.0";
    std::string app_version = "v1.2.2";
    uint64_t timestamp = 0;
    std::string original_image_name;
    uint32_t image_width = 0;
    uint32_t image_height = 0;
    uint32_t layer_count = 0;
    int active_layer_index = 0;
    ViewportState view;
    DepthState depth;
};

struct SerializedLayerEntry {
    uint32_t id = 0;
    std::string name;
    int type = 0; // 0 = Raster, 1 = Text, 2 = MaskOverlay
    bool visible = true;
    int blend_mode = 0;
    float opacity = 1.0f;
    float fill_opacity = 1.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float rotation_deg = 0.0f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    bool lock_alpha = false;
    bool lock_pixels = false;
    bool lock_position = false;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> png_bytes;

    // Text layer fields (used when type == 1):
    std::string text_utf8;
    std::string font_family = "Segoe UI";
    float font_size = 32.0f;
    float text_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    int text_align = 0;
    bool font_bold = false;
    bool font_italic = false;
    bool font_underline = false;
    bool font_strikethrough = false;
    float line_spacing = 1.15f;
    float tracking = 0.0f;
    float stroke_width = 0.0f;
    float stroke_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float bg_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool shadow_enabled = false;
    float shadow_offset_x = 2.0f;
    float shadow_offset_y = 2.0f;
    float shadow_blur = 3.0f;
    float shadow_color[4] = { 0.0f, 0.0f, 0.0f, 0.6f };
};

// returns true if filename ends with .shaderlab or .slab (case-insensitive)
bool is_project_file(const std::wstring &path);

// saves a complete self-contained .shaderlab archive:
// - manifest.json (metadata, camera, image properties)
// - preset.ini (provided via preset_ini_bytes)
// - source_image.<ext> (exact original image bytes)
// - depth.sldepth (companion depth sidecar if present)
bool save_project(
    const std::wstring &project_path,
    const std::wstring &source_image_path,
    const std::vector<uint8_t> &preset_ini_bytes,
    const ViewportState &view_state,
    const DepthState &depth_state,
    std::wstring &out_error
);

// Loads/extracts a .shaderlab archive into a destination or temp folder:
// - Extracts source_image to out_image_path
// - Extracts preset.ini to out_preset_path
// - Extracts depth.sldepth if present
// - Fills out_manifest with parsed view and depth values
bool load_project(
    const std::wstring &project_path,
    const std::wstring &temp_extract_dir,
    std::wstring &out_image_path,
    std::wstring &out_preset_path,
    ProjectManifest &out_manifest,
    std::wstring &out_error
);

// v2.0 self-contained archive with multi-layer RGBA PNGs + text/transform metadata.
// Reads both v1.0 and v2.0 archives seamlessly (v1 archives return empty out_layers).
bool save_project_v2(
    const std::wstring &project_path,
    const std::wstring &source_image_path,
    const std::vector<uint8_t> &preset_ini_bytes,
    const ViewportState &view_state,
    const DepthState &depth_state,
    const std::vector<SerializedLayerEntry> &layers,
    uint32_t canvas_width,
    uint32_t canvas_height,
    int active_layer_index,
    std::wstring &out_error
);

bool load_project_v2(
    const std::wstring &project_path,
    const std::wstring &temp_extract_dir,
    std::wstring &out_image_path,
    std::wstring &out_preset_path,
    ProjectManifest &out_manifest,
    std::vector<SerializedLayerEntry> &out_layers,
    std::wstring &out_error
);

} // namespace project_file
