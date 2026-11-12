#pragma once

#include <imgui.h>
#include <cstdint>
#include <vector>

/**
 * State and math helpers for the Rotate Tool (R) and Camera Raw-style
 * interactive Horizon Alignment line.
 */
struct HorizonAlignState {
    bool is_drawing_line = false;
    ImVec2 start_screen{ 0.0f, 0.0f };
    ImVec2 end_screen{ 0.0f, 0.0f };
    float preview_delta_deg = 0.0f;
    bool auto_scale_to_fit = false; // "Constrain to Image" auto-zoom toggle
    bool flip_horizontal = false;
    bool flip_vertical = false;

    /**
     * Computes straightening delta angle in degrees so segment (p1 -> p2)
     * becomes level (or vertical if |angle| > 45 degrees).
     */
    static float compute_horizon_straighten_angle(const ImVec2 &p1, const ImVec2 &p2) noexcept;

    /**
     * Normalizes an angle in degrees into [0, 360).
     */
    static float normalize_angle_360(float deg) noexcept;

    /**
     * Normalizes an angle in degrees into [-180, 180].
     */
    static float normalize_angle_signed180(float deg) noexcept;

    /**
     * Computes the minimum scale factor >= 1.0 needed so a rotated image (img_w x img_h)
     * completely fills an unrotated frame of the same dimensions without empty corners.
     */
    static float compute_constrain_zoom_scale(uint32_t img_w, uint32_t img_h, float angle_deg) noexcept;
};

/**
 * State and geometry helpers for the interactive Crop Tool (C) with 9 aspect presets.
 */
struct CropToolState {
    enum class Preset : int {
        Freeform = 0,
        Original = 1,
        Square_1_1 = 2,
        Wide_16_9 = 3,
        Vertical_9_16 = 4,
        Photo_4_3 = 5,
        DSLR_3_2 = 6,
        Portrait_4_5 = 7,
        Cinema_21_9 = 8
    };

    static constexpr int kPresetCount = 9;

    struct PixelRect {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
    };

    bool active = false;
    Preset preset = Preset::Freeform;
    bool aspect_swapped = false;

    // Normalized [0..1] crop marquee bounds inside current document space.
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 1.0f;
    float y1 = 1.0f;

    // -1 = none, 0..3 = corners (TL, TR, BR, BL), 4..7 = edges (T, R, B, L), 8 = move inside
    int active_handle = -1;

    static const char *get_preset_name(Preset p) noexcept;
    static float get_preset_aspect(Preset p, uint32_t orig_w, uint32_t orig_h) noexcept;

    float get_effective_aspect(uint32_t orig_w, uint32_t orig_h) const noexcept;
    void apply_preset_ratio(uint32_t doc_w, uint32_t doc_h) noexcept;
    void swap_orientation(uint32_t doc_w, uint32_t doc_h) noexcept;
    void reset(uint32_t doc_w = 0, uint32_t doc_h = 0) noexcept;

    void drag_handle(int handle, float delta_norm_x, float delta_norm_y,
                     uint32_t doc_w, uint32_t doc_h) noexcept;

    PixelRect get_pixel_rect(uint32_t doc_w, uint32_t doc_h) const noexcept;
};

/**
 * Crops an RGBA8 pixel buffer (4 bytes per pixel) to [crop_x, crop_y, crop_w, crop_h].
 * Areas outside source bounds are zero-filled with transparent black (0, 0, 0, 0).
 */
bool crop_rgba_buffer(const std::vector<uint8_t> &src, uint32_t src_w, uint32_t src_h,
                      int crop_x, int crop_y, int crop_w, int crop_h,
                      std::vector<uint8_t> &out_dst);

/**
 * Crops a single-channel float buffer (e.g., 3D depth map) to [crop_x, crop_y, crop_w, crop_h].
 */
bool crop_float_buffer(const std::vector<float> &src, uint32_t src_w, uint32_t src_h,
                       int crop_x, int crop_y, int crop_w, int crop_h,
                       std::vector<float> &out_dst);
