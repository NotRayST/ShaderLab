#pragma once

#include <imgui.h>
#include <cstdint>
#include <vector>

/**
 * Software raster alpha eraser for pixel layers (Feature #12).
 * Uses smoothstep radial hardness falloff and per-stroke max-accumulation
 * so overlapping dabs within a single stroke respect constant opacity.
 */
class RasterEraser {
public:
    float brush_size = 40.0f;     // Diameter in pixels (1 .. 500)
    float brush_hardness = 0.75f; // 0.0 = soft feather, 1.0 = hard edge
    float brush_opacity = 1.00f;  // 0.01 .. 1.00

    struct DirtyRect {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0; // inclusive
        int y1 = 0; // inclusive
        bool valid = false;

        void expand(int nx0, int ny0, int nx1, int ny1) noexcept;
    };

    /**
     * Snapshots layer RGBA pixels and initializes the per-stroke falloff map.
     */
    void begin_stroke(const std::vector<uint8_t> &layer_rgba, uint32_t w, uint32_t h);

    /**
     * Erases alpha along segment p0 -> p1 in layer-local pixel coordinates.
     * Optionally outputs the dirty rect modified by this call.
     */
    void paint_segment(std::vector<uint8_t> &io_layer_rgba, const ImVec2 &p0, const ImVec2 &p1,
                       DirtyRect *out_segment_dirty = nullptr);

    /**
     * Ends the current stroke, returns true if a stroke was active, and moves out
     * the pre-stroke RGBA snapshot for undo history.
     */
    bool end_stroke(std::vector<uint8_t> &out_snapshot_before_stroke);

    /**
     * Cancels/clears stroke buffers without returning a snapshot.
     */
    void cancel_stroke() noexcept;

    bool is_stroke_active() const noexcept { return m_stroke_active; }
    uint32_t width() const noexcept { return m_width; }
    uint32_t height() const noexcept { return m_height; }
    const DirtyRect &get_stroke_dirty_rect() const noexcept { return m_dirty_rect; }

private:
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_stroke_active = false;
    DirtyRect m_dirty_rect{};
    std::vector<uint8_t> m_snapshot_before_stroke;
    std::vector<float> m_stroke_map;
};
