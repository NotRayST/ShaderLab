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
    ViewportState view;
    DepthState depth;
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

} // namespace project_file
