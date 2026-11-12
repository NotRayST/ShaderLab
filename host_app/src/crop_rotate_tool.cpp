#include "crop_rotate_tool.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kMinSegLenSq = 25.0f; // Ignore clicks shorter than 5 px
constexpr float kMinCropNorm = 0.01f;
} // namespace

float HorizonAlignState::compute_horizon_straighten_angle(const ImVec2 &p1, const ImVec2 &p2) noexcept {
    const float dx = p2.x - p1.x;
    const float dy = p2.y - p1.y;
    if (dx * dx + dy * dy < kMinSegLenSq) {
        return 0.0f;
    }

    float angle_deg = std::atan2(dy, dx) * kRadToDeg;
    if (angle_deg > 90.0f) {
        angle_deg -= 180.0f;
    }
    if (angle_deg < -90.0f) {
        angle_deg += 180.0f;
    }

    // Align near-vertical architectural lines (> 45 deg) to +-90 deg, else to 0 deg.
    if (std::abs(angle_deg) > 45.0f) {
        const float target = (angle_deg > 0.0f) ? 90.0f : -90.0f;
        return -(angle_deg - target);
    }
    return -angle_deg;
}

float HorizonAlignState::normalize_angle_360(float deg) noexcept {
    float res = std::fmod(deg, 360.0f);
    if (res < 0.0f) {
        res += 360.0f;
    }
    return res;
}

float HorizonAlignState::normalize_angle_signed180(float deg) noexcept {
    float res = normalize_angle_360(deg);
    if (res > 180.0f) {
        res -= 360.0f;
    }
    return res;
}

float HorizonAlignState::compute_constrain_zoom_scale(uint32_t img_w, uint32_t img_h, float angle_deg) noexcept {
    if (img_w == 0 || img_h == 0) {
        return 1.0f;
    }
    const float theta = std::abs(normalize_angle_signed180(angle_deg)) * kDegToRad;
    const float c = std::abs(std::cos(theta));
    const float s = std::abs(std::sin(theta));
    const float aspect = static_cast<float>(img_w) / static_cast<float>(img_h);
    const float scale_x = c + (1.0f / aspect) * s;
    const float scale_y = aspect * s + c;
    return (std::max)(1.0f, (std::max)(scale_x, scale_y));
}

const char *CropToolState::get_preset_name(Preset p) noexcept {
    switch (p) {
    case Preset::Freeform:      return "Freeform (Unconstrained)";
    case Preset::Original:      return "Original Ratio";
    case Preset::Square_1_1:    return "1:1 (Square)";
    case Preset::Wide_16_9:     return "16:9 (Widescreen)";
    case Preset::Vertical_9_16: return "9:16 (Vertical / Reel)";
    case Preset::Photo_4_3:     return "4:3 (Standard Photo)";
    case Preset::DSLR_3_2:      return "3:2 (35mm DSLR)";
    case Preset::Portrait_4_5:  return "4:5 (Portrait Social)";
    case Preset::Cinema_21_9:   return "21:9 (Ultrawide Cinema)";
    default:                    return "Freeform (Unconstrained)";
    }
}

float CropToolState::get_preset_aspect(Preset p, uint32_t orig_w, uint32_t orig_h) noexcept {
    switch (p) {
    case Preset::Original:      return (orig_h > 0) ? (static_cast<float>(orig_w) / static_cast<float>(orig_h)) : 1.0f;
    case Preset::Square_1_1:    return 1.0f;
    case Preset::Wide_16_9:     return 16.0f / 9.0f;
    case Preset::Vertical_9_16: return 9.0f / 16.0f;
    case Preset::Photo_4_3:     return 4.0f / 3.0f;
    case Preset::DSLR_3_2:      return 3.0f / 2.0f;
    case Preset::Portrait_4_5:  return 4.0f / 5.0f;
    case Preset::Cinema_21_9:   return 21.0f / 9.0f;
    default:                    return 0.0f;
    }
}

float CropToolState::get_effective_aspect(uint32_t orig_w, uint32_t orig_h) const noexcept {
    const float base = get_preset_aspect(preset, orig_w, orig_h);
    if (base <= 0.0f) {
        return 0.0f;
    }
    return aspect_swapped ? (1.0f / base) : base;
}

void CropToolState::apply_preset_ratio(uint32_t doc_w, uint32_t doc_h) noexcept {
    const float target_aspect = get_effective_aspect(doc_w, doc_h);
    if (target_aspect <= 0.0f || doc_w == 0 || doc_h == 0) {
        return;
    }

    const float doc_aspect = static_cast<float>(doc_w) / static_cast<float>(doc_h);
    float norm_w = 1.0f;
    float norm_h = 1.0f;
    if (target_aspect > doc_aspect) {
        norm_w = 1.0f;
        norm_h = doc_aspect / target_aspect;
    } else {
        norm_h = 1.0f;
        norm_w = target_aspect / doc_aspect;
    }
    x0 = 0.5f - norm_w * 0.5f;
    x1 = 0.5f + norm_w * 0.5f;
    y0 = 0.5f - norm_h * 0.5f;
    y1 = 0.5f + norm_h * 0.5f;
}

void CropToolState::swap_orientation(uint32_t doc_w, uint32_t doc_h) noexcept {
    if (doc_w == 0 || doc_h == 0) {
        return;
    }
    if (preset == Preset::Wide_16_9) {
        preset = Preset::Vertical_9_16;
        aspect_swapped = false;
        apply_preset_ratio(doc_w, doc_h);
        return;
    }
    if (preset == Preset::Vertical_9_16) {
        preset = Preset::Wide_16_9;
        aspect_swapped = false;
        apply_preset_ratio(doc_w, doc_h);
        return;
    }
    if (preset == Preset::Freeform) {
        const float cx = (x0 + x1) * 0.5f;
        const float cy = (y0 + y1) * 0.5f;
        const float cur_px_w = (x1 - x0) * static_cast<float>(doc_w);
        const float cur_px_h = (y1 - y0) * static_cast<float>(doc_h);
        float new_norm_w = cur_px_h / static_cast<float>(doc_w);
        float new_norm_h = cur_px_w / static_cast<float>(doc_h);
        const float fit_scale = (std::max)(1.0f, (std::max)(new_norm_w, new_norm_h));
        new_norm_w /= fit_scale;
        new_norm_h /= fit_scale;
        x0 = std::clamp(cx - new_norm_w * 0.5f, 0.0f, 1.0f - new_norm_w);
        y0 = std::clamp(cy - new_norm_h * 0.5f, 0.0f, 1.0f - new_norm_h);
        x1 = x0 + new_norm_w;
        y1 = y0 + new_norm_h;
        return;
    }

    aspect_swapped = !aspect_swapped;
    apply_preset_ratio(doc_w, doc_h);
}

void CropToolState::reset(uint32_t doc_w, uint32_t doc_h) noexcept {
    active_handle = -1;
    aspect_swapped = false;
    x0 = 0.0f;
    y0 = 0.0f;
    x1 = 1.0f;
    y1 = 1.0f;
    if (preset != Preset::Freeform && doc_w > 0 && doc_h > 0) {
        apply_preset_ratio(doc_w, doc_h);
    }
}

void CropToolState::drag_handle(int handle, float delta_norm_x, float delta_norm_y,
                                uint32_t doc_w, uint32_t doc_h) noexcept {
    if (handle < 0 || handle > 8 || doc_w == 0 || doc_h == 0) {
        return;
    }

    if (handle == 8) {
        const float w = x1 - x0;
        const float h = y1 - y0;
        const float nx0 = std::clamp(x0 + delta_norm_x, 0.0f, 1.0f - w);
        const float ny0 = std::clamp(y0 + delta_norm_y, 0.0f, 1.0f - h);
        x0 = nx0;
        y0 = ny0;
        x1 = nx0 + w;
        y1 = ny0 + h;
        return;
    }

    const float target_aspect = get_effective_aspect(doc_w, doc_h);
    const float doc_aspect = static_cast<float>(doc_w) / static_cast<float>(doc_h);

    float nx0 = x0;
    float ny0 = y0;
    float nx1 = x1;
    float ny1 = y1;

    switch (handle) {
    case 0: // TL
        nx0 = std::clamp(x0 + delta_norm_x, 0.0f, x1 - kMinCropNorm);
        ny0 = std::clamp(y0 + delta_norm_y, 0.0f, y1 - kMinCropNorm);
        break;
    case 1: // TR
        nx1 = std::clamp(x1 + delta_norm_x, x0 + kMinCropNorm, 1.0f);
        ny0 = std::clamp(y0 + delta_norm_y, 0.0f, y1 - kMinCropNorm);
        break;
    case 2: // BR
        nx1 = std::clamp(x1 + delta_norm_x, x0 + kMinCropNorm, 1.0f);
        ny1 = std::clamp(y1 + delta_norm_y, y0 + kMinCropNorm, 1.0f);
        break;
    case 3: // BL
        nx0 = std::clamp(x0 + delta_norm_x, 0.0f, x1 - kMinCropNorm);
        ny1 = std::clamp(y1 + delta_norm_y, y0 + kMinCropNorm, 1.0f);
        break;
    case 4: // Top
        ny0 = std::clamp(y0 + delta_norm_y, 0.0f, y1 - kMinCropNorm);
        break;
    case 5: // Right
        nx1 = std::clamp(x1 + delta_norm_x, x0 + kMinCropNorm, 1.0f);
        break;
    case 6: // Bottom
        ny1 = std::clamp(y1 + delta_norm_y, y0 + kMinCropNorm, 1.0f);
        break;
    case 7: // Left
        nx0 = std::clamp(x0 + delta_norm_x, 0.0f, x1 - kMinCropNorm);
        break;
    default:
        break;
    }

    if (target_aspect > 0.0f) {
        // Ratio in normalized coordinates: (w_norm * doc_aspect) / h_norm == target_aspect
        const float norm_ratio = target_aspect / doc_aspect;
        const bool vertical_edge = (handle == 4 || handle == 6);
        const bool horiz_edge = (handle == 5 || handle == 7);

        if (vertical_edge) {
            float h = (std::max)(kMinCropNorm, ny1 - ny0);
            float w = h * norm_ratio;
            if (w > 1.0f) {
                w = 1.0f;
                h = w / norm_ratio;
                if (handle == 4) ny0 = ny1 - h;
                else ny1 = ny0 + h;
            }
            const float cx = (x0 + x1) * 0.5f;
            nx0 = std::clamp(cx - w * 0.5f, 0.0f, 1.0f - w);
            nx1 = nx0 + w;
        } else if (horiz_edge) {
            float w = (std::max)(kMinCropNorm, nx1 - nx0);
            float h = w / norm_ratio;
            if (h > 1.0f) {
                h = 1.0f;
                w = h * norm_ratio;
                if (handle == 7) nx0 = nx1 - w;
                else nx1 = nx0 + w;
            }
            const float cy = (y0 + y1) * 0.5f;
            ny0 = std::clamp(cy - h * 0.5f, 0.0f, 1.0f - h);
            ny1 = ny0 + h;
        } else {
            // Corner drag: anchor the opposite corner cleanly.
            float w = (std::max)(kMinCropNorm, nx1 - nx0);
            float h = w / norm_ratio;
            if (handle == 0 || handle == 1) {
                const float max_h = ny1;
                if (h > max_h) {
                    h = max_h;
                    w = h * norm_ratio;
                }
                ny0 = ny1 - h;
            } else {
                const float max_h = 1.0f - ny0;
                if (h > max_h) {
                    h = max_h;
                    w = h * norm_ratio;
                }
                ny1 = ny0 + h;
            }
            if (handle == 0 || handle == 3) {
                const float max_w = nx1;
                if (w > max_w) {
                    w = max_w;
                    h = w / norm_ratio;
                    if (handle == 0) ny0 = ny1 - h;
                    else ny1 = ny0 + h;
                }
                nx0 = nx1 - w;
            } else {
                const float max_w = 1.0f - nx0;
                if (w > max_w) {
                    w = max_w;
                    h = w / norm_ratio;
                    if (handle == 1) ny0 = ny1 - h;
                    else ny1 = ny0 + h;
                }
                nx1 = nx0 + w;
            }
        }
    }

    x0 = std::clamp(nx0, 0.0f, 1.0f - kMinCropNorm);
    y0 = std::clamp(ny0, 0.0f, 1.0f - kMinCropNorm);
    x1 = std::clamp(nx1, x0 + kMinCropNorm, 1.0f);
    y1 = std::clamp(ny1, y0 + kMinCropNorm, 1.0f);
}

CropToolState::PixelRect CropToolState::get_pixel_rect(uint32_t doc_w, uint32_t doc_h) const noexcept {
    PixelRect r;
    if (doc_w == 0 || doc_h == 0) {
        return r;
    }
    const int dw = static_cast<int>(doc_w);
    const int dh = static_cast<int>(doc_h);

    const int px0 = std::clamp(static_cast<int>(std::round(x0 * static_cast<float>(dw))), 0, dw - 1);
    const int py0 = std::clamp(static_cast<int>(std::round(y0 * static_cast<float>(dh))), 0, dh - 1);
    const int px1 = std::clamp(static_cast<int>(std::round(x1 * static_cast<float>(dw))), px0 + 1, dw);
    const int py1 = std::clamp(static_cast<int>(std::round(y1 * static_cast<float>(dh))), py0 + 1, dh);

    r.x = px0;
    r.y = py0;
    r.w = px1 - px0;
    r.h = py1 - py0;
    return r;
}

bool crop_rgba_buffer(const std::vector<uint8_t> &src, uint32_t src_w, uint32_t src_h,
                      int crop_x, int crop_y, int crop_w, int crop_h,
                      std::vector<uint8_t> &out_dst) {
    if (crop_w <= 0 || crop_h <= 0 || src_w == 0 || src_h == 0) {
        return false;
    }
    if (src.size() < static_cast<size_t>(src_w) * src_h * 4) {
        return false;
    }

    out_dst.assign(static_cast<size_t>(crop_w) * crop_h * 4, 0u);
    const int dst_x_start = (std::max)(0, -crop_x);
    const int dst_y_start = (std::max)(0, -crop_y);
    const int dst_x_end = (std::min)(crop_w, static_cast<int>(src_w) - crop_x);
    const int dst_y_end = (std::min)(crop_h, static_cast<int>(src_h) - crop_y);
    if (dst_x_end <= dst_x_start || dst_y_end <= dst_y_start) {
        return true;
    }

    const size_t copy_bytes = static_cast<size_t>(dst_x_end - dst_x_start) * 4;
    for (int dy = dst_y_start; dy < dst_y_end; ++dy) {
        const int sy = crop_y + dy;
        const int sx = crop_x + dst_x_start;
        const size_t src_off = (static_cast<size_t>(sy) * src_w + static_cast<size_t>(sx)) * 4;
        const size_t dst_off = (static_cast<size_t>(dy) * crop_w + static_cast<size_t>(dst_x_start)) * 4;
        std::memcpy(out_dst.data() + dst_off, src.data() + src_off, copy_bytes);
    }
    return true;
}

bool crop_float_buffer(const std::vector<float> &src, uint32_t src_w, uint32_t src_h,
                       int crop_x, int crop_y, int crop_w, int crop_h,
                       std::vector<float> &out_dst) {
    if (crop_w <= 0 || crop_h <= 0 || src_w == 0 || src_h == 0) {
        return false;
    }
    if (src.size() < static_cast<size_t>(src_w) * src_h) {
        return false;
    }

    out_dst.assign(static_cast<size_t>(crop_w) * crop_h, 0.0f);
    const int dst_x_start = (std::max)(0, -crop_x);
    const int dst_y_start = (std::max)(0, -crop_y);
    const int dst_x_end = (std::min)(crop_w, static_cast<int>(src_w) - crop_x);
    const int dst_y_end = (std::min)(crop_h, static_cast<int>(src_h) - crop_y);
    if (dst_x_end <= dst_x_start || dst_y_end <= dst_y_start) {
        return true;
    }

    const size_t copy_count = static_cast<size_t>(dst_x_end - dst_x_start);
    for (int dy = dst_y_start; dy < dst_y_end; ++dy) {
        const int sy = crop_y + dy;
        const int sx = crop_x + dst_x_start;
        const size_t src_off = static_cast<size_t>(sy) * src_w + static_cast<size_t>(sx);
        const size_t dst_off = static_cast<size_t>(dy) * crop_w + static_cast<size_t>(dst_x_start);
        std::memcpy(out_dst.data() + dst_off, src.data() + src_off, copy_count * sizeof(float));
    }
    return true;
}
