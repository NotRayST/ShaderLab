#include "raster_eraser.h"

#include <algorithm>
#include <cmath>

void RasterEraser::DirtyRect::expand(int nx0, int ny0, int nx1, int ny1) noexcept {
    if (nx0 > nx1 || ny0 > ny1) {
        return;
    }
    if (!valid) {
        x0 = nx0;
        y0 = ny0;
        x1 = nx1;
        y1 = ny1;
        valid = true;
    } else {
        x0 = (std::min)(x0, nx0);
        y0 = (std::min)(y0, ny0);
        x1 = (std::max)(x1, nx1);
        y1 = (std::max)(y1, ny1);
    }
}

void RasterEraser::begin_stroke(const std::vector<uint8_t> &layer_rgba, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || layer_rgba.size() != static_cast<size_t>(w) * h * 4) {
        cancel_stroke();
        return;
    }
    m_width = w;
    m_height = h;
    m_snapshot_before_stroke = layer_rgba;
    m_stroke_map.assign(static_cast<size_t>(w) * h, 0.0f);
    m_dirty_rect = DirtyRect{};
    m_stroke_active = true;
}

void RasterEraser::paint_segment(std::vector<uint8_t> &io_layer_rgba, const ImVec2 &p0, const ImVec2 &p1,
                                 DirtyRect *out_segment_dirty) {
    if (out_segment_dirty) {
        *out_segment_dirty = DirtyRect{};
    }
    if (!m_stroke_active || m_width == 0 || m_height == 0 ||
        io_layer_rgba.size() != static_cast<size_t>(m_width) * m_height * 4 ||
        m_snapshot_before_stroke.size() != io_layer_rgba.size() ||
        m_stroke_map.size() != static_cast<size_t>(m_width) * m_height) {
        return;
    }

    const float clamped_size = std::clamp(brush_size, 1.0f, 500.0f);
    const float radius = (std::max)(0.5f, clamped_size * 0.5f);
    const float opacity = std::clamp(brush_opacity, 0.0f, 1.0f);
    if (opacity <= 0.0f) {
        return;
    }

    const int x0 = (std::max)(0, static_cast<int>(std::floor((std::min)(p0.x, p1.x) - radius - 1.0f)));
    const int y0 = (std::max)(0, static_cast<int>(std::floor((std::min)(p0.y, p1.y) - radius - 1.0f)));
    const int x1 = (std::min)(static_cast<int>(m_width) - 1,
                              static_cast<int>(std::ceil((std::max)(p0.x, p1.x) + radius + 1.0f)));
    const int y1 = (std::min)(static_cast<int>(m_height) - 1,
                              static_cast<int>(std::ceil((std::max)(p0.y, p1.y) + radius + 1.0f)));
    if (x0 > x1 || y0 > y1) {
        return;
    }

    const float abx = p1.x - p0.x;
    const float aby = p1.y - p0.y;
    const float ab_len2 = abx * abx + aby * aby;
    const float inner_r = radius * std::clamp(brush_hardness, 0.0f, 0.999f);
    const float feather_span = (std::max)(0.001f, radius - inner_r);

    bool any_modified = false;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float t = (ab_len2 > 1e-6f)
                ? std::clamp(((px - p0.x) * abx + (py - p0.y) * aby) / ab_len2, 0.0f, 1.0f)
                : 0.0f;
            const float dx = px - (p0.x + abx * t);
            const float dy = py - (p0.y + aby * t);
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > radius) {
                continue;
            }

            float falloff = 1.0f;
            if (dist > inner_r) {
                const float u = std::clamp((dist - inner_r) / feather_span, 0.0f, 1.0f);
                falloff = 1.0f - (u * u * (3.0f - 2.0f * u));
            }

            const size_t idx = static_cast<size_t>(y) * m_width + static_cast<size_t>(x);
            if (falloff > m_stroke_map[idx]) {
                m_stroke_map[idx] = falloff;
                const float erase_factor = std::clamp(falloff * opacity, 0.0f, 1.0f);
                const uint8_t orig_a = m_snapshot_before_stroke[idx * 4 + 3];
                const uint8_t new_a = static_cast<uint8_t>(
                    std::clamp(std::round(static_cast<float>(orig_a) * (1.0f - erase_factor)), 0.0f, 255.0f));
                io_layer_rgba[idx * 4 + 3] = new_a;
                any_modified = true;
            }
        }
    }

    if (any_modified) {
        m_dirty_rect.expand(x0, y0, x1, y1);
        if (out_segment_dirty) {
            out_segment_dirty->expand(x0, y0, x1, y1);
        }
    }
}

bool RasterEraser::end_stroke(std::vector<uint8_t> &out_snapshot_before_stroke) {
    if (!m_stroke_active) {
        out_snapshot_before_stroke.clear();
        return false;
    }
    out_snapshot_before_stroke = std::move(m_snapshot_before_stroke);
    cancel_stroke();
    return true;
}

void RasterEraser::cancel_stroke() noexcept {
    m_stroke_active = false;
    m_width = 0;
    m_height = 0;
    m_dirty_rect = DirtyRect{};
    m_snapshot_before_stroke.clear();
    m_stroke_map.clear();
}
