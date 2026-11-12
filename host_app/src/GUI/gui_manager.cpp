#include "gui_manager.h"
#include "theme.h"
#include "menu_bar.h"
#include "tool_strip.h"
#include "tab_bar.h"
#include "left_panel.h"
#include "right_panel.h"
#include "canvas.h"
#include "status_bar.h"
#include "../erase_tool.h"
#include "../resource.h"

#include <windowsx.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace gui {

namespace {

constexpr float kPi = 3.14159265358979323846f;

const char *active_tool_to_string(ActiveTool tool) noexcept {
    switch (tool) {
    case ActiveTool::Move:        return "Move";
    case ActiveTool::Hand:        return "Hand";
    case ActiveTool::Rotate:      return "Rotate";
    case ActiveTool::Crop:        return "Crop";
    case ActiveTool::RasterErase: return "RasterErase";
    case ActiveTool::AiInpaint:   return "AiInpaint";
    case ActiveTool::Text:        return "Text";
    case ActiveTool::Upscale:     return "Upscale";
    case ActiveTool::Depth:       return "Depth";
    case ActiveTool::Split:       return "Split";
    default:                      return "Hand";
    }
}

void compute_viewport_metrics(float view_x, float view_y, float view_w, float view_h,
                              uint32_t img_w, uint32_t img_h,
                              float zoom, float pan_x, float pan_y, float angle_deg,
                              float &out_cx, float &out_cy, float &out_eff_zoom,
                              float &out_cos, float &out_sin) {
    out_cx = view_x + view_w * 0.5f;
    out_cy = view_y + view_h * 0.5f;
    const float rad = angle_deg * (kPi / 180.0f);
    out_cos = std::cos(rad);
    out_sin = std::sin(rad);
    out_eff_zoom = 1.0f;
    if (img_w > 0 && img_h > 0) {
        const float abs_c = std::abs(out_cos);
        const float abs_s = std::abs(out_sin);
        const float rot_w = static_cast<float>(img_w) * abs_c + static_cast<float>(img_h) * abs_s;
        const float rot_h = static_cast<float>(img_w) * abs_s + static_cast<float>(img_h) * abs_c;
        const float fit_scale = (std::min)(
            view_w / (std::max)(1.0f, rot_w),
            view_h / (std::max)(1.0f, rot_h));
        out_eff_zoom = (std::max)(0.001f, zoom * fit_scale);
    }
    (void)pan_x;
    (void)pan_y;
}

float point_segment_dist(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float abx = b.x - a.x;
    const float aby = b.y - a.y;
    const float len2 = abx * abx + aby * aby;
    if (len2 <= 1e-6f) {
        return std::hypot(p.x - a.x, p.y - a.y);
    }
    const float t = std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / len2, 0.0f, 1.0f);
    return std::hypot(p.x - (a.x + abx * t), p.y - (a.y + aby * t));
}

} // namespace

GuiManager::GuiManager() = default;

GuiManager::~GuiManager() {
    shutdown();
}

bool GuiManager::initialize(HWND hwnd, ID3D11Device *device, ID3D11DeviceContext *context) {
    if (m_initialized) return true;

    m_hwnd = hwnd;
    m_device = device;
    m_context = context;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.IniFilename = nullptr; // Stateless / clean startup

    apply_photoshop_gray_theme();

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    m_cursor_grab = LoadCursorW(hInst, MAKEINTRESOURCEW(IDC_CURSOR_GRAB));
    m_cursor_grip = LoadCursorW(hInst, MAKEINTRESOURCEW(IDC_CURSOR_GRIP));

    if (!ImGui_ImplWin32_Init(hwnd)) {
        ImGui::DestroyContext();
        return false;
    }

    if (!ImGui_ImplDX11_Init(device, context)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    add_sample_effects(m_effects);
    // Feature #3: No shader toggled on by default at launch
    for (auto &fx : m_effects) {
        fx.enabled = false;
    }

    m_initialized = true;
    return true;
}

void GuiManager::shutdown() {
    if (!m_initialized) return;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    m_initialized = false;
    m_hwnd = nullptr;
    m_device = nullptr;
    m_context = nullptr;
}

void GuiManager::mark_active_document_dirty() {
    if (m_active_tab >= 0 && m_active_tab < static_cast<int>(m_tabs.size())) {
        m_tabs[m_active_tab].is_dirty = true;
    }
    if (on_document_dirty) {
        on_document_dirty();
    }
}

void GuiManager::set_active_tool(ActiveTool tool) {
    const ActiveTool prev = m_active_tool;
    m_active_tool = tool;
    m_erase_active = (tool == ActiveTool::AiInpaint);
    m_before_after = (tool == ActiveTool::Split);
    m_depth_peek = (tool == ActiveTool::Depth);

    if (prev != tool) {
        m_horizon_state.is_drawing_line = false;
        m_is_dragging_move = false;
        m_is_dragging_crop = false;
        m_crop_state.active_handle = -1;
        if (m_is_raster_erasing) {
            std::vector<uint8_t> discarded;
            m_raster_eraser.end_stroke(discarded);
            m_is_raster_erasing = false;
        }
    }

    if (tool == ActiveTool::Crop) {
        m_crop_state.active = true;
        if (m_crop_state.x1 <= m_crop_state.x0 || m_crop_state.y1 <= m_crop_state.y0) {
            m_crop_state.reset(m_last_img_w, m_last_img_h);
        } else if (m_crop_state.preset != CropToolState::Preset::Freeform && m_last_img_w > 0 && m_last_img_h > 0) {
            m_crop_state.apply_preset_ratio(m_last_img_w, m_last_img_h);
        }
    } else {
        m_crop_state.active = false;
    }

    if (tool == ActiveTool::AiInpaint) {
        m_right_panel_tab = 2;
    } else if (tool == ActiveTool::Depth) {
        m_right_panel_tab = 1;
    } else if (tool == ActiveTool::Upscale) {
        m_right_panel_tab = 3;
    } else if (tool == ActiveTool::Text) {
        m_right_panel_tab = 4;
    } else if (tool == ActiveTool::Hand && m_right_panel_tab != 5) {
        m_right_panel_tab = 0;
    }

    EraseTool &erase_tool = EraseTool::get();
    erase_tool.set_active(tool == ActiveTool::AiInpaint);
    if (tool == ActiveTool::AiInpaint) {
        erase_tool.brush_radius = m_erase_radius;
    }
}

void GuiManager::select_right_panel_tab(int tab_idx) {
    m_right_panel_tab = tab_idx;
    if (tab_idx == 0) {
        if (m_active_tool == ActiveTool::AiInpaint || m_active_tool == ActiveTool::Depth ||
            m_active_tool == ActiveTool::Upscale || m_active_tool == ActiveTool::Text) {
            set_active_tool(ActiveTool::Hand);
        }
    } else if (tab_idx == 1) {
        set_active_tool(ActiveTool::Depth);
    } else if (tab_idx == 2) {
        set_active_tool(ActiveTool::AiInpaint);
    } else if (tab_idx == 3) {
        set_active_tool(ActiveTool::Upscale);
    } else if (tab_idx == 4) {
        set_active_tool(ActiveTool::Text);
    }
}

void GuiManager::trigger_apply_crop() {
    if (m_last_img_w == 0 || m_last_img_h == 0) return;
    const CropToolState::PixelRect rect = m_crop_state.get_pixel_rect(m_last_img_w, m_last_img_h);
    if (rect.w <= 0 || rect.h <= 0) return;

    m_pending_crop_rect = rect;
    m_req_apply_crop = true;
    if (on_crop_document) {
        on_crop_document(rect.x, rect.y, rect.w, rect.h);
    }
    mark_active_document_dirty();
    m_crop_state.reset();
    set_active_tool(ActiveTool::Hand);
}

void GuiManager::cancel_crop_tool() {
    m_crop_state.reset();
    m_is_dragging_crop = false;
    set_active_tool(ActiveTool::Hand);
}

void GuiManager::apply_rotation_delta(float new_angle_deg) {
    const float normalized_360 = HorizonAlignState::normalize_angle_360(new_angle_deg);
    m_target_viewport_angle = normalized_360;
    m_last_angle = normalized_360;
    m_rotate_angle_deg = HorizonAlignState::normalize_angle_signed180(normalized_360);
    m_req_set_viewport_angle = true;
    if (on_rotate_apply) {
        on_rotate_apply(normalized_360, m_horizon_state.auto_scale_to_fit);
    }
    mark_active_document_dirty();
}

void GuiManager::nudge_active_layer(float dx, float dy) {
    if (m_active_layer_view.lock_position) return;
    if (std::abs(dx) < 1e-6f && std::abs(dy) < 1e-6f) return;

    m_active_layer_view.offset_x += dx;
    m_active_layer_view.offset_y += dy;
    m_pending_move_dx += dx;
    m_pending_move_dy += dy;
    m_has_pending_move_delta = true;
    if (on_move_layer_delta) {
        on_move_layer_delta(dx, dy);
    }
    mark_active_document_dirty();
}

void GuiManager::screen_to_doc_px(float sx, float sy, float &out_dx, float &out_dy) const {
    float cx = 0.0f, cy = 0.0f, eff_zoom = 1.0f, cos_a = 1.0f, sin_a = 0.0f;
    compute_viewport_metrics(m_canvas_view_x, m_canvas_view_y, m_canvas_view_w, m_canvas_view_h,
                             m_last_img_w, m_last_img_h, m_last_zoom, m_last_pan_x, m_last_pan_y,
                             m_last_angle, cx, cy, eff_zoom, cos_a, sin_a);

    const float px = (sx - cx) / eff_zoom;
    const float py = (sy - cy) / eff_zoom;
    const float qx = px * cos_a + py * sin_a;
    const float qy = -px * sin_a + py * cos_a;
    out_dx = qx + static_cast<float>(m_last_img_w) * 0.5f + m_last_pan_x;
    out_dy = qy + static_cast<float>(m_last_img_h) * 0.5f + m_last_pan_y;
}

int GuiManager::hit_test_crop_handle(float sx, float sy) const {
    if (m_active_tool != ActiveTool::Crop || m_last_img_w == 0 || m_last_img_h == 0) {
        return -1;
    }

    float cx = 0.0f, cy = 0.0f, eff_zoom = 1.0f, cos_a = 1.0f, sin_a = 0.0f;
    compute_viewport_metrics(m_canvas_view_x, m_canvas_view_y, m_canvas_view_w, m_canvas_view_h,
                             m_last_img_w, m_last_img_h, m_last_zoom, m_last_pan_x, m_last_pan_y,
                             m_last_angle, cx, cy, eff_zoom, cos_a, sin_a);
    const float hw = static_cast<float>(m_last_img_w) * 0.5f;
    const float hh = static_cast<float>(m_last_img_h) * 0.5f;
    auto to_scr = [&](float dx, float dy) -> ImVec2 {
        const float qx = dx - (hw + m_last_pan_x);
        const float qy = dy - (hh + m_last_pan_y);
        return ImVec2(cx + (qx * cos_a - qy * sin_a) * eff_zoom,
                      cy + (qx * sin_a + qy * cos_a) * eff_zoom);
    };

    const float fw = static_cast<float>(m_last_img_w);
    const float fh = static_cast<float>(m_last_img_h);
    const ImVec2 tl = to_scr(m_crop_state.x0 * fw, m_crop_state.y0 * fh);
    const ImVec2 tr = to_scr(m_crop_state.x1 * fw, m_crop_state.y0 * fh);
    const ImVec2 br = to_scr(m_crop_state.x1 * fw, m_crop_state.y1 * fh);
    const ImVec2 bl = to_scr(m_crop_state.x0 * fw, m_crop_state.y1 * fh);

    const ImVec2 pt(sx, sy);
    constexpr float kCornerRadius = 12.0f;
    if (std::hypot(pt.x - tl.x, pt.y - tl.y) <= kCornerRadius) return 0;
    if (std::hypot(pt.x - tr.x, pt.y - tr.y) <= kCornerRadius) return 1;
    if (std::hypot(pt.x - br.x, pt.y - br.y) <= kCornerRadius) return 2;
    if (std::hypot(pt.x - bl.x, pt.y - bl.y) <= kCornerRadius) return 3;

    constexpr float kEdgeDist = 8.0f;
    if (point_segment_dist(pt, tl, tr) <= kEdgeDist) return 4;
    if (point_segment_dist(pt, tr, br) <= kEdgeDist) return 5;
    if (point_segment_dist(pt, br, bl) <= kEdgeDist) return 6;
    if (point_segment_dist(pt, bl, tl) <= kEdgeDist) return 7;

    float doc_x = 0.0f, doc_y = 0.0f;
    screen_to_doc_px(sx, sy, doc_x, doc_y);
    const float nx = doc_x / fw;
    const float ny = doc_y / fh;
    if (nx >= m_crop_state.x0 && nx <= m_crop_state.x1 &&
        ny >= m_crop_state.y0 && ny <= m_crop_state.y1) {
        return 8;
    }
    return -1;
}

void GuiManager::request_inpaint() {
    if (EraseTool::get().is_processing() || m_inpaint_is_busy) return;
    if (!EraseTool::get().has_mask_edits() && m_erase_strokes.empty()) return;
    m_req_inpaint = true;
    m_inpaint_status_text = "Running LaMa Big HD inpainting...";
    m_inpaint_is_busy = true;
}

void GuiManager::on_erase_stroke_finished(bool was_subtract) {
    if (!was_subtract && EraseTool::get().auto_apply &&
        !EraseTool::get().is_processing() && EraseTool::get().has_mask_edits()) {
        request_inpaint();
    }
}

void GuiManager::push_erase_undo() {
    if (m_erase_undo_stack.size() >= 30) {
        m_erase_undo_stack.erase(m_erase_undo_stack.begin());
    }
    m_erase_undo_stack.push_back(m_erase_strokes);
    m_erase_redo_stack.clear();
}

bool GuiManager::undo_erase_stroke() {
    const bool tool_undone = EraseTool::get().undo_stroke();
    if (tool_undone && m_device) {
        EraseTool::get().update_gpu_texture(m_device);
    }
    bool gui_undone = false;
    if (m_erase_undo_stack.empty()) {
        if (!m_erase_strokes.empty()) {
            m_erase_redo_stack.push_back(std::move(m_erase_strokes));
            m_erase_strokes.clear();
            gui_undone = true;
        }
    } else {
        m_erase_redo_stack.push_back(std::move(m_erase_strokes));
        m_erase_strokes = std::move(m_erase_undo_stack.back());
        m_erase_undo_stack.pop_back();
        gui_undone = true;
    }
    return tool_undone || gui_undone;
}

bool GuiManager::redo_erase_stroke() {
    const bool tool_redone = EraseTool::get().redo_stroke();
    if (tool_redone && m_device) {
        EraseTool::get().update_gpu_texture(m_device);
    }
    bool gui_redone = false;
    if (!m_erase_redo_stack.empty()) {
        m_erase_undo_stack.push_back(std::move(m_erase_strokes));
        m_erase_strokes = std::move(m_erase_redo_stack.back());
        m_erase_redo_stack.pop_back();
        gui_redone = true;
    }
    return tool_redone || gui_redone;
}

void GuiManager::clear_erase_strokes() {
    if (!m_erase_strokes.empty()) {
        push_erase_undo();
    }
    m_erase_strokes.clear();
    m_current_stroke.clear();
    m_is_painting_mask = false;
    m_is_right_click_subtract = false;
    m_last_paint_px = ImVec2(-1.0f, -1.0f);
    EraseTool::get().clear_mask();
    if (m_device) {
        EraseTool::get().update_gpu_texture(m_device);
    }
}

void GuiManager::sync_erase_strokes_to_tool(uint32_t img_w, uint32_t img_h) {
    EraseTool &tool = EraseTool::get();
    tool.clear_mask();
    if (img_w > 0 && img_h > 0) {
        tool.ensure_mask_size(img_w, img_h);
        for (const auto &stroke : m_erase_strokes) {
            if (stroke.empty()) continue;
            tool.push_undo();
            ImVec2 prev(stroke[0].x * static_cast<float>(img_w), stroke[0].y * static_cast<float>(img_h));
            tool.draw_brush_dab(static_cast<int>(std::round(prev.x)), static_cast<int>(std::round(prev.y)),
                                static_cast<int>(std::round(m_erase_radius)), 255);
            for (size_t i = 1; i < stroke.size(); ++i) {
                const ImVec2 curr(stroke[i].x * static_cast<float>(img_w), stroke[i].y * static_cast<float>(img_h));
                tool.draw_brush_line(prev, curr, m_erase_radius, false);
                prev = curr;
            }
        }
        if (m_device) {
            tool.update_gpu_texture(m_device);
        }
    }
}

void GuiManager::subtract_brush_dab(float uv_x, float uv_y, float radius_uv_x, float radius_uv_y) {
    if (m_erase_strokes.empty()) return;
    const float rx2 = (radius_uv_x > 0.0f) ? (radius_uv_x * radius_uv_x) : 0.0001f;
    const float ry2 = (radius_uv_y > 0.0f) ? (radius_uv_y * radius_uv_y) : 0.0001f;

    std::vector<std::vector<ImVec2>> new_strokes;
    for (const auto &stroke : m_erase_strokes) {
        std::vector<ImVec2> current_seg;
        for (const auto &pt : stroke) {
            const float dx = pt.x - uv_x;
            const float dy = pt.y - uv_y;
            const bool inside = ((dx * dx) / rx2 + (dy * dy) / ry2) <= 1.0f;
            if (!inside) {
                current_seg.push_back(pt);
            } else if (!current_seg.empty()) {
                new_strokes.push_back(std::move(current_seg));
                current_seg.clear();
            }
        }
        if (!current_seg.empty()) {
            new_strokes.push_back(std::move(current_seg));
        }
    }
    m_erase_strokes = std::move(new_strokes);
}

GuiManager::CursorHitState GuiManager::compute_cursor_hit_state(POINT pt) const {
    CursorHitState hit{};
    RECT client_rc = {};
    GetClientRect(m_hwnd, &client_rc);
    hit.screen_w = static_cast<float>(client_rc.right - client_rc.left);
    const float screen_h = static_cast<float>(client_rc.bottom - client_rc.top);
    const float menu_h = get_menu_bar_height();
    const float tool_options_h = get_tool_options_height();
    const float toolbar_w = get_toolbar_width();
    const float status_bar_h = get_status_bar_height();

    const float left_w = get_left_panel_width(static_cast<uint32_t>(hit.screen_w));
    const float right_w = get_right_panel_width(static_cast<uint32_t>(hit.screen_w));
    hit.viewable_x = toolbar_w + left_w;
    hit.viewable_y = menu_h + tool_options_h;
    hit.viewable_w = hit.screen_w - hit.viewable_x - right_w;
    hit.viewable_h = screen_h - hit.viewable_y - status_bar_h;

    const float left_splitter_x = toolbar_w + left_w;
    const float right_splitter_x = hit.screen_w - right_w;
    const bool in_vert = (pt.y >= hit.viewable_y && pt.y < hit.viewable_y + hit.viewable_h);

    hit.near_left_splitter = (!m_hide_panels && in_vert && std::abs(static_cast<float>(pt.x) - left_splitter_x) <= 5.0f);
    hit.near_right_splitter = (!m_hide_panels && in_vert && std::abs(static_cast<float>(pt.x) - right_splitter_x) <= 5.0f);

    const float split_norm = std::clamp(0.5f + m_before_after_split, 0.05f, 0.95f);
    const float split_x = hit.viewable_x + hit.viewable_w * split_norm;
    hit.near_split_line = m_before_after && in_vert && (std::abs(static_cast<float>(pt.x) - split_x) <= 8.0f);

    hit.in_canvas = (pt.x > hit.viewable_x + 4.0f && pt.x < (hit.viewable_x + hit.viewable_w) - 4.0f && in_vert);
    hit.popup_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    hit.in_client = PtInRect(&client_rc, pt) != FALSE;
    hit.crop_handle = (hit.in_canvas && m_active_tool == ActiveTool::Crop)
        ? hit_test_crop_handle(static_cast<float>(pt.x), static_cast<float>(pt.y))
        : -1;
    return hit;
}

void GuiManager::apply_cursor_for_state(const CursorHitState &hit) {
    if (m_resizing_left || m_resizing_right || m_is_dragging_split) {
        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
        m_active_cursor_name = "sizewe";
        return;
    }

    if (is_viewport_panning()) {
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        if (!m_cursor_grab) m_cursor_grab = LoadCursorW(hInst, MAKEINTRESOURCEW(IDC_CURSOR_GRAB));
        if (!m_cursor_grip) m_cursor_grip = LoadCursorW(hInst, MAKEINTRESOURCEW(IDC_CURSOR_GRIP));
        SetCursor(m_cursor_grip ? m_cursor_grip : LoadCursor(nullptr, IDC_SIZEALL));
        m_active_cursor_name = "grip";
        return;
    }

    if (is_viewport_capturing()) {
        SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
        m_active_cursor_name = "sizeall";
        return;
    }

    if (m_is_painting_mask || m_is_raster_erasing) {
        SetCursor(nullptr);
        m_active_cursor_name = "none";
        return;
    }

    if (m_horizon_state.is_drawing_line) {
        SetCursor(LoadCursor(nullptr, IDC_CROSS));
        m_active_cursor_name = "cross";
        return;
    }

    if (m_is_dragging_move) {
        SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
        m_active_cursor_name = "sizeall";
        return;
    }

    if (!hit.popup_open && (hit.near_left_splitter || hit.near_right_splitter || hit.near_split_line)) {
        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
        m_active_cursor_name = "sizewe";
        return;
    }

    const bool space_held = m_space_held || ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0);
    if (hit.in_canvas && has_loaded_image() && !hit.popup_open) {
        const bool want_text = ImGui::GetIO().WantTextInput;
        if (m_active_tool == ActiveTool::Hand || (space_held && !want_text)) {
            HINSTANCE hInst = GetModuleHandleW(nullptr);
            if (!m_cursor_grab) m_cursor_grab = LoadCursorW(hInst, MAKEINTRESOURCEW(IDC_CURSOR_GRAB));
            if (!m_cursor_grip) m_cursor_grip = LoadCursorW(hInst, MAKEINTRESOURCEW(IDC_CURSOR_GRIP));
            SetCursor(m_cursor_grab ? m_cursor_grab : LoadCursor(nullptr, IDC_HAND));
            m_active_cursor_name = "grab";
            return;
        }
        if (m_active_tool == ActiveTool::AiInpaint || m_active_tool == ActiveTool::RasterErase) {
            SetCursor(nullptr);
            m_active_cursor_name = "none";
            return;
        }
        if (m_active_tool == ActiveTool::Move) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            m_active_cursor_name = "sizeall";
            return;
        }
        if (m_active_tool == ActiveTool::Rotate) {
            SetCursor(LoadCursor(nullptr, IDC_CROSS));
            m_active_cursor_name = "cross";
            return;
        }
        if (m_active_tool == ActiveTool::Text) {
            SetCursor(LoadCursor(nullptr, IDC_IBEAM));
            m_active_cursor_name = "ibeam";
            return;
        }
        if (m_active_tool == ActiveTool::Crop) {
            const int ch = m_is_dragging_crop ? m_crop_state.active_handle : hit.crop_handle;
            switch (ch) {
            case 0:
            case 2: SetCursor(LoadCursor(nullptr, IDC_SIZENWSE)); m_active_cursor_name = "sizenwse"; return;
            case 1:
            case 3: SetCursor(LoadCursor(nullptr, IDC_SIZENESW)); m_active_cursor_name = "sizenesw"; return;
            case 4:
            case 6: SetCursor(LoadCursor(nullptr, IDC_SIZENS));   m_active_cursor_name = "sizens"; return;
            case 5:
            case 7: SetCursor(LoadCursor(nullptr, IDC_SIZEWE));   m_active_cursor_name = "sizewe"; return;
            case 8: SetCursor(LoadCursor(nullptr, IDC_SIZEALL));  m_active_cursor_name = "sizeall"; return;
            default: SetCursor(LoadCursor(nullptr, IDC_ARROW));   m_active_cursor_name = "arrow"; return;
            }
        }
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        m_active_cursor_name = "arrow";
        return;
    }

    HCURSOR win32_cursor = LoadCursor(nullptr, IDC_ARROW);
    const char *cursor_name = "arrow";
    switch (ImGui::GetMouseCursor()) {
    case ImGuiMouseCursor_None:       win32_cursor = nullptr; cursor_name = "none"; break;
    case ImGuiMouseCursor_Arrow:      win32_cursor = LoadCursor(nullptr, IDC_ARROW); cursor_name = "arrow"; break;
    case ImGuiMouseCursor_TextInput:  win32_cursor = LoadCursor(nullptr, IDC_IBEAM); cursor_name = "ibeam"; break;
    case ImGuiMouseCursor_ResizeAll:  win32_cursor = LoadCursor(nullptr, IDC_SIZEALL); cursor_name = "sizeall"; break;
    case ImGuiMouseCursor_ResizeEW:   win32_cursor = LoadCursor(nullptr, IDC_SIZEWE); cursor_name = "sizewe"; break;
    case ImGuiMouseCursor_ResizeNS:   win32_cursor = LoadCursor(nullptr, IDC_SIZENS); cursor_name = "sizens"; break;
    case ImGuiMouseCursor_ResizeNESW: win32_cursor = LoadCursor(nullptr, IDC_SIZENESW); cursor_name = "sizenesw"; break;
    case ImGuiMouseCursor_ResizeNWSE: win32_cursor = LoadCursor(nullptr, IDC_SIZENWSE); cursor_name = "sizenwse"; break;
    case ImGuiMouseCursor_Hand:       win32_cursor = LoadCursor(nullptr, IDC_HAND); cursor_name = "hand"; break;
    case ImGuiMouseCursor_NotAllowed: win32_cursor = LoadCursor(nullptr, IDC_NO); cursor_name = "no"; break;
    default: break;
    }
    SetCursor(win32_cursor);
    m_active_cursor_name = cursor_name;
}

void GuiManager::begin_capture() {
    if (m_hwnd && GetCapture() != m_hwnd) {
        SetCapture(m_hwnd);
    }
    m_owns_capture = (m_hwnd && GetCapture() == m_hwnd);
}

void GuiManager::end_capture() {
    if (m_owns_capture) {
        m_owns_capture = false;
        ReleaseCapture();
    }
}

void GuiManager::cancel_all_drags() {
    m_resizing_left = false;
    m_resizing_right = false;
    m_is_dragging_split = false;
    if (m_is_painting_mask) {
        if (!m_current_stroke.empty()) {
            m_erase_strokes.push_back(std::move(m_current_stroke));
            m_current_stroke.clear();
        }
        m_is_painting_mask = false;
    }
    m_is_right_click_subtract = false;
    m_last_paint_px = ImVec2(-1.0f, -1.0f);
    m_horizon_state.is_drawing_line = false;
    m_is_dragging_move = false;
    m_move_locked_axis = 0;
    m_is_dragging_crop = false;
    m_crop_state.active_handle = -1;
    if (m_is_raster_erasing) {
        std::vector<uint8_t> before_rgba;
        if (m_raster_eraser.end_stroke(before_rgba) && on_push_layer_pixel_undo) {
            on_push_layer_pixel_undo(before_rgba);
        }
        m_is_raster_erasing = false;
    }
    vp_cancel_input();
}

bool GuiManager::input_hook(void *user_data, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *self = static_cast<GuiManager *>(user_data);
    if (!self || !self->m_initialized) return false;

    auto is_client_mouse_msg = [](UINT m) {
        return (m >= WM_MOUSEMOVE && m <= WM_MBUTTONDBLCLK) ||
               (m >= WM_XBUTTONDOWN && m <= WM_XBUTTONDBLCLK);
    };

    if (msg == WM_MOUSELEAVE) {
        self->m_cursor_in_client = false;
    } else if (msg == WM_KILLFOCUS || msg == WM_CANCELMODE) {
        if (msg == WM_KILLFOCUS) {
            self->m_space_held = false;
            self->m_depth_key_held = false;
            self->vp_set_space_pan(false);
            self->m_cursor_in_client = false;
        }
        self->cancel_all_drags();
        self->end_capture();
    } else if (msg == WM_CAPTURECHANGED) {
        if (self->m_owns_capture && reinterpret_cast<HWND>(lParam) != self->m_hwnd) {
            self->m_owns_capture = false;
            self->cancel_all_drags();
        }
    }

    POINT pt = self->m_last_mouse_pt;
    if (is_client_mouse_msg(msg)) {
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        self->m_last_mouse_pt = pt;
    } else if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        ScreenToClient(self->m_hwnd, &pt);
        self->m_last_mouse_pt = pt;
    } else if (msg == WM_SETCURSOR && !self->m_cursor_in_client) {
        POINT cur = {};
        if (GetCursorPos(&cur) && ScreenToClient(self->m_hwnd, &cur)) {
            pt = cur;
            self->m_last_mouse_pt = pt;
        }
    }

    const CursorHitState hit = self->compute_cursor_hit_state(pt);
    const bool active_drag = self->has_active_drag();

    if (msg == WM_SETCURSOR) {
        if (LOWORD(lParam) != HTCLIENT && !active_drag) {
            self->m_cursor_in_client = false;
            self->m_active_cursor_name = "nonclient";
            return false;
        }
        self->m_cursor_in_client = (LOWORD(lParam) == HTCLIENT);
        self->apply_cursor_for_state(hit);
        return true;
    }

    auto sync_hover_cursor = [&]() {
        if (self->m_cursor_in_client || self->has_active_drag()) {
            self->apply_cursor_for_state(self->compute_cursor_hit_state(self->m_last_mouse_pt));
        }
    };

    if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
        if (wParam == VK_SPACE) {
            self->m_space_held = false;
            self->vp_set_space_pan(false);
        } else if (wParam == 'D' || wParam == 'd') {
            self->m_depth_key_held = false;
        }
        ImGui_ImplWin32_WndProcHandler(self->m_hwnd, msg, wParam, lParam);
        if (wParam == VK_SPACE || wParam == 'D' || wParam == 'd') {
            sync_hover_cursor();
        }
        return false;
    }

    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if ((wParam == 'D' || wParam == 'd') && !ImGui::GetIO().WantTextInput && !hit.popup_open) {
            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (!ctrl) {
                self->m_depth_key_held = true;
                return true;
            }
        }

        if (wParam == VK_SPACE) {
            ImGui_ImplWin32_WndProcHandler(self->m_hwnd, msg, wParam, lParam);
            if (!ImGui::GetIO().WantTextInput && !hit.popup_open) {
                self->m_space_held = true;
                self->vp_set_space_pan(true);
                sync_hover_cursor();
                return true;
            }
            return true;
        }

        if (ImGui_ImplWin32_WndProcHandler(self->m_hwnd, msg, wParam, lParam)) {
            return true;
        }

        const ImGuiIO &io = ImGui::GetIO();
        if (msg == WM_KEYDOWN && !io.WantTextInput && !hit.popup_open) {
            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

            if (wParam == VK_TAB && !ctrl && !alt) {
                self->toggle_panels_hidden();
                sync_hover_cursor();
                return true;
            } else if (wParam == 'S' && ctrl && !shift && !alt) {
                self->m_req_save_project = true;
                return true;
            } else if (wParam == 'S' && ctrl && shift && !alt) {
                self->m_req_save_project_as = true;
                return true;
            } else if (wParam == 'E' && ctrl && !shift && !alt) {
                self->m_req_export_image = true;
                return true;
            } else if (wParam == 'E' && ctrl && shift && !alt) {
                self->m_req_export_image_as = true;
                return true;
            } else if (wParam == VK_OEM_COMMA && ctrl && !alt) {
                self->select_right_panel_tab(5);
                self->m_req_open_prefs = true;
                return true;
            } else if (wParam == 'V' && !ctrl && !alt) {
                self->set_active_tool(ActiveTool::Move);
                sync_hover_cursor();
                return true;
            } else if (wParam == 'H' && !ctrl && !alt) {
                self->set_active_tool(ActiveTool::Hand);
                sync_hover_cursor();
                return true;
            } else if (wParam == 'R' && !ctrl && !alt) {
                self->set_active_tool(ActiveTool::Rotate);
                sync_hover_cursor();
                return true;
            } else if (wParam == 'C' && !ctrl && !alt) {
                self->set_active_tool(ActiveTool::Crop);
                sync_hover_cursor();
                return true;
            } else if (wParam == 'E' && !ctrl && !alt) {
                self->set_active_tool(ActiveTool::RasterErase);
                sync_hover_cursor();
                return true;
            } else if (wParam == 'J' && !ctrl && !alt) {
                self->set_active_tool(
                    self->m_active_tool == ActiveTool::AiInpaint ? ActiveTool::Hand : ActiveTool::AiInpaint);
                sync_hover_cursor();
                return true;
            } else if (wParam == 'T' && !ctrl && !alt) {
                self->set_active_tool(ActiveTool::Text);
                sync_hover_cursor();
                return true;
            } else if (wParam == 'U' && !ctrl && !alt) {
                self->set_active_tool(ActiveTool::Upscale);
                sync_hover_cursor();
                return true;
            } else if (wParam == 'B' && !ctrl && !alt) {
                self->set_active_tool(
                    self->m_active_tool == ActiveTool::Split ? ActiveTool::Hand : ActiveTool::Split);
                sync_hover_cursor();
                return true;
            } else if (wParam == VK_OEM_5 && !ctrl) {
                self->m_master_bypass = !self->m_master_bypass;
                return true;
            } else if (wParam == '0' && ctrl) {
                self->m_req_fit_view = true;
                return true;
            } else if (wParam == '1' && ctrl) {
                self->m_req_100_view = true;
                return true;
            } else if (wParam == '2' && ctrl) {
                self->m_req_200_view = true;
                return true;
            } else if ((wParam == VK_OEM_PLUS || wParam == VK_ADD) && ctrl) {
                self->m_req_zoom_in = true;
                return true;
            } else if ((wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) && ctrl) {
                self->m_req_zoom_out = true;
                return true;
            } else if (wParam == VK_F1) {
                self->m_show_shortcuts_dialog = true;
                return true;
            } else if (wParam == VK_F5) {
                self->m_effects.clear();
                add_sample_effects(self->m_effects);
                for (auto &fx : self->m_effects) {
                    fx.enabled = false;
                }
                self->m_selected_effect_idx = 0;
                return true;
            } else if (wParam == VK_OEM_4) {
                self->m_erase_radius = (std::max)(2.0f, self->m_erase_radius - 4.0f);
                EraseTool::get().brush_radius = self->m_erase_radius;
                self->m_raster_eraser.brush_size = (std::max)(1.0f, self->m_raster_eraser.brush_size - 4.0f);
                return true;
            } else if (wParam == VK_OEM_6) {
                self->m_erase_radius = (std::min)(256.0f, self->m_erase_radius + 4.0f);
                EraseTool::get().brush_radius = self->m_erase_radius;
                self->m_raster_eraser.brush_size = (std::min)(500.0f, self->m_raster_eraser.brush_size + 4.0f);
                return true;
            } else if (self->m_active_tool == ActiveTool::Move && !ctrl && !alt &&
                       (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN)) {
                const float step = shift ? 10.0f : 1.0f;
                if (wParam == VK_LEFT)  self->nudge_active_layer(-step, 0.0f);
                if (wParam == VK_RIGHT) self->nudge_active_layer(+step, 0.0f);
                if (wParam == VK_UP)    self->nudge_active_layer(0.0f, -step);
                if (wParam == VK_DOWN)  self->nudge_active_layer(0.0f, +step);
                return true;
            } else if (wParam == 'Z' && ctrl && !shift &&
                       (self->m_active_tool == ActiveTool::AiInpaint || EraseTool::get().can_undo_stroke())) {
                self->undo_erase_stroke();
                return true;
            } else if (((wParam == 'Y' && ctrl) || (wParam == 'Z' && ctrl && shift)) &&
                       (self->m_active_tool == ActiveTool::AiInpaint || EraseTool::get().can_redo_stroke())) {
                self->redo_erase_stroke();
                return true;
            } else if (wParam == VK_RETURN && !ctrl && !shift && !alt) {
                if (self->m_active_tool == ActiveTool::Crop) {
                    self->trigger_apply_crop();
                    sync_hover_cursor();
                    return true;
                }
                if (self->m_active_tool == ActiveTool::AiInpaint &&
                    EraseTool::get().has_mask_edits() &&
                    !EraseTool::get().is_processing()) {
                    self->request_inpaint();
                    return true;
                }
            } else if (wParam == VK_ESCAPE) {
                if (self->m_active_tool == ActiveTool::Crop) {
                    self->cancel_crop_tool();
                    sync_hover_cursor();
                    return true;
                }
                const bool has_mask = !self->m_erase_strokes.empty() || EraseTool::get().has_mask_edits();
                if (self->m_active_tool == ActiveTool::AiInpaint && has_mask) {
                    self->clear_erase_strokes();
                    self->m_req_clear_mask = true;
                    return true;
                }
                if (self->m_active_tool != ActiveTool::Hand) {
                    self->set_active_tool(ActiveTool::Hand);
                    sync_hover_cursor();
                    return true;
                }
                if (has_mask) {
                    self->clear_erase_strokes();
                    self->m_req_clear_mask = true;
                    return true;
                }
            }
        }

        if (!io.WantTextInput && !hit.popup_open) {
            if (self->vp_handle_input(msg, wParam, lParam)) {
                sync_hover_cursor();
                return true;
            }
        }
        return io.WantTextInput || io.WantCaptureKeyboard;
    }

    if (msg == WM_CHAR) {
        if (ImGui_ImplWin32_WndProcHandler(self->m_hwnd, msg, wParam, lParam)) return true;
        return ImGui::GetIO().WantTextInput;
    }

    const bool is_mouse_msg = is_client_mouse_msg(msg) || msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL;

    // 1. Continue UI splitters
    if (self->m_resizing_left || self->m_resizing_right || self->m_is_dragging_split) {
        if (!is_mouse_msg) return false;
        if (msg == WM_MOUSEMOVE) {
            if (self->m_resizing_left) {
                const float new_w = static_cast<float>(pt.x) - self->get_toolbar_width();
                self->m_left_panel_width = std::clamp(new_w, 240.0f, hit.screen_w * 0.45f);
            } else if (self->m_resizing_right) {
                const float new_w = hit.screen_w - static_cast<float>(pt.x);
                self->m_right_panel_width = std::clamp(new_w, 300.0f, hit.screen_w * 0.45f);
            } else if (self->m_is_dragging_split) {
                const float rel_x = (static_cast<float>(pt.x) - hit.viewable_x) /
                                    (hit.viewable_w > 0.0f ? hit.viewable_w : 1.0f);
                self->m_before_after_split = std::clamp(rel_x - 0.5f, -0.45f, 0.45f);
            }
            self->apply_cursor_for_state(hit);
            return true;
        }
        if (msg == WM_LBUTTONUP) {
            self->m_resizing_left = false;
            self->m_resizing_right = false;
            self->m_is_dragging_split = false;
            self->end_capture();
            self->apply_cursor_for_state(hit);
            return true;
        }
        return true;
    }

    auto compute_brush_uv = [&](float &uv_x, float &uv_y, float &rad_uv_x, float &rad_uv_y, bool &over_image) {
        float img_px_x = 0.0f, img_px_y = 0.0f;
        self->screen_to_doc_px(static_cast<float>(pt.x), static_cast<float>(pt.y), img_px_x, img_px_y);
        const float margin_px = self->m_erase_radius + 4.0f;
        over_image = (self->m_last_img_w > 0 && self->m_last_img_h > 0 &&
                      img_px_x >= -margin_px && img_px_x <= static_cast<float>(self->m_last_img_w) + margin_px &&
                      img_px_y >= -margin_px && img_px_y <= static_cast<float>(self->m_last_img_h) + margin_px);
        const float clamped_x = std::clamp(img_px_x, 0.0f, static_cast<float>(self->m_last_img_w));
        const float clamped_y = std::clamp(img_px_y, 0.0f, static_cast<float>(self->m_last_img_h));
        uv_x = (self->m_last_img_w > 0) ? (clamped_x / static_cast<float>(self->m_last_img_w)) : 0.0f;
        uv_y = (self->m_last_img_h > 0) ? (clamped_y / static_cast<float>(self->m_last_img_h)) : 0.0f;
        rad_uv_x = (self->m_last_img_w > 0) ? (self->m_erase_radius / static_cast<float>(self->m_last_img_w)) : 0.01f;
        rad_uv_y = (self->m_last_img_h > 0) ? (self->m_erase_radius / static_cast<float>(self->m_last_img_h)) : 0.01f;
    };

    // 2. Continue active AI Inpaint mask paint / right-click subtract
    if (self->m_is_painting_mask) {
        if (!is_mouse_msg) return false;
        float uv_x = 0.0f, uv_y = 0.0f, rad_uv_x = 0.01f, rad_uv_y = 0.01f;
        bool over_image = false;
        compute_brush_uv(uv_x, uv_y, rad_uv_x, rad_uv_y, over_image);
        const bool is_sub = self->m_erase_subtract || self->m_is_right_click_subtract ||
                            ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);

        if (msg == WM_MOUSEMOVE) {
            self->m_cursor_in_client = hit.in_client;
            if (is_sub) {
                self->subtract_brush_dab(uv_x, uv_y, rad_uv_x, rad_uv_y);
            } else if (self->m_current_stroke.empty() ||
                       std::hypot(self->m_current_stroke.back().x - uv_x,
                                  self->m_current_stroke.back().y - uv_y) > 0.001f) {
                self->m_current_stroke.push_back(ImVec2(uv_x, uv_y));
            }
            if (self->m_last_img_w > 0 && self->m_last_img_h > 0) {
                EraseTool &tool = EraseTool::get();
                tool.ensure_mask_size(self->m_last_img_w, self->m_last_img_h);
                tool.brush_radius = self->m_erase_radius;
                const float px = uv_x * static_cast<float>(self->m_last_img_w);
                const float py = uv_y * static_cast<float>(self->m_last_img_h);
                const ImVec2 curr_px(px, py);
                if (self->m_last_paint_px.x >= 0.0f && self->m_last_paint_px.y >= 0.0f) {
                    tool.draw_brush_line(self->m_last_paint_px, curr_px, self->m_erase_radius, is_sub);
                } else {
                    tool.draw_brush_dab(static_cast<int>(std::round(px)), static_cast<int>(std::round(py)),
                                        static_cast<int>(std::round(self->m_erase_radius)), is_sub ? 0 : 255);
                }
                self->m_last_paint_px = curr_px;
                if (self->m_device) {
                    tool.update_gpu_texture(self->m_device);
                }
            }
            self->apply_cursor_for_state(hit);
            return true;
        }
        if (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP) {
            const bool finished_sub = is_sub || (msg == WM_RBUTTONUP);
            self->m_is_painting_mask = false;
            self->m_is_right_click_subtract = false;
            self->m_last_paint_px = ImVec2(-1.0f, -1.0f);
            if (!finished_sub && !self->m_current_stroke.empty()) {
                self->m_erase_strokes.push_back(self->m_current_stroke);
                self->m_current_stroke.clear();
            } else {
                self->m_current_stroke.clear();
            }
            self->end_capture();
            self->apply_cursor_for_state(hit);
            self->on_erase_stroke_finished(finished_sub);
            return true;
        }
        return true;
    }

    // 3. Continue active Rotate Horizon Align drag (Feature #6)
    if (self->m_horizon_state.is_drawing_line) {
        if (!is_mouse_msg) return false;
        if (msg == WM_MOUSEMOVE) {
            self->m_cursor_in_client = hit.in_client;
            self->m_horizon_state.end_screen = ImVec2(static_cast<float>(pt.x), static_cast<float>(pt.y));
            self->m_horizon_state.preview_delta_deg = HorizonAlignState::compute_horizon_straighten_angle(
                self->m_horizon_state.start_screen, self->m_horizon_state.end_screen);
            self->apply_cursor_for_state(hit);
            return true;
        }
        if (msg == WM_LBUTTONUP) {
            self->m_horizon_state.end_screen = ImVec2(static_cast<float>(pt.x), static_cast<float>(pt.y));
            self->m_horizon_state.is_drawing_line = false;
            const float delta_deg = HorizonAlignState::compute_horizon_straighten_angle(
                self->m_horizon_state.start_screen, self->m_horizon_state.end_screen);
            self->end_capture();
            if (std::abs(delta_deg) > 0.001f) {
                self->apply_rotation_delta(self->m_last_angle + delta_deg);
            }
            self->apply_cursor_for_state(hit);
            return true;
        }
        return true;
    }

    // 4. Continue active Move Tool drag (Feature #8)
    if (self->m_is_dragging_move) {
        if (!is_mouse_msg) return false;
        if (msg == WM_MOUSEMOVE) {
            self->m_cursor_in_client = hit.in_client;
            float cx = 0.0f, cy = 0.0f, eff_zoom = 1.0f, cos_a = 1.0f, sin_a = 0.0f;
            compute_viewport_metrics(self->m_canvas_view_x, self->m_canvas_view_y,
                                     self->m_canvas_view_w, self->m_canvas_view_h,
                                     self->m_last_img_w, self->m_last_img_h,
                                     self->m_last_zoom, self->m_last_pan_x, self->m_last_pan_y,
                                     self->m_last_angle, cx, cy, eff_zoom, cos_a, sin_a);
            const float dsx = static_cast<float>(pt.x) - self->m_move_drag_last_screen.x;
            const float dsy = static_cast<float>(pt.y) - self->m_move_drag_last_screen.y;
            self->m_move_drag_last_screen = ImVec2(static_cast<float>(pt.x), static_cast<float>(pt.y));

            float dx = (dsx * cos_a + dsy * sin_a) / eff_zoom;
            float dy = (-dsx * sin_a + dsy * cos_a) / eff_zoom;
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (shift) {
                self->m_move_drag_accum.x += dx;
                self->m_move_drag_accum.y += dy;
                if (self->m_move_locked_axis == 0 &&
                    (std::abs(self->m_move_drag_accum.x) > 3.0f || std::abs(self->m_move_drag_accum.y) > 3.0f)) {
                    self->m_move_locked_axis =
                        (std::abs(self->m_move_drag_accum.x) >= std::abs(self->m_move_drag_accum.y)) ? 1 : 2;
                }
                if (self->m_move_locked_axis == 1) dy = 0.0f;
                else if (self->m_move_locked_axis == 2) dx = 0.0f;
            } else {
                self->m_move_locked_axis = 0;
            }
            self->nudge_active_layer(dx, dy);
            self->apply_cursor_for_state(hit);
            return true;
        }
        if (msg == WM_LBUTTONUP) {
            self->m_is_dragging_move = false;
            self->m_move_locked_axis = 0;
            self->end_capture();
            self->apply_cursor_for_state(hit);
            return true;
        }
        return true;
    }

    // 5. Continue active Raster Alpha Eraser drag (Feature #12)
    if (self->m_is_raster_erasing) {
        if (!is_mouse_msg) return false;
        if (msg == WM_MOUSEMOVE) {
            self->m_cursor_in_client = hit.in_client;
            if (self->on_get_active_layer_pixels) {
                LayerPixelView pv = self->on_get_active_layer_pixels();
                if (pv.rgba_pixels && !pv.lock_pixels && !pv.lock_alpha && !pv.is_text_layer) {
                    float doc_x = 0.0f, doc_y = 0.0f;
                    self->screen_to_doc_px(static_cast<float>(pt.x), static_cast<float>(pt.y), doc_x, doc_y);
                    const ImVec2 curr_local(doc_x - pv.offset_x, doc_y - pv.offset_y);
                    RasterEraser::DirtyRect dirty{};
                    self->m_raster_eraser.paint_segment(*pv.rgba_pixels, self->m_last_raster_erase_px, curr_local, &dirty);
                    self->m_last_raster_erase_px = curr_local;
                    if (dirty.valid && self->on_commit_layer_pixels) {
                        self->on_commit_layer_pixels(dirty.x0, dirty.y0, dirty.x1 - dirty.x0 + 1, dirty.y1 - dirty.y0 + 1);
                    }
                }
            }
            self->apply_cursor_for_state(hit);
            return true;
        }
        if (msg == WM_LBUTTONUP) {
            self->m_is_raster_erasing = false;
            self->m_last_raster_erase_px = ImVec2(-1.0f, -1.0f);
            std::vector<uint8_t> before_rgba;
            if (self->m_raster_eraser.end_stroke(before_rgba) && self->on_push_layer_pixel_undo) {
                self->on_push_layer_pixel_undo(before_rgba);
            }
            self->mark_active_document_dirty();
            self->end_capture();
            self->apply_cursor_for_state(hit);
            return true;
        }
        return true;
    }

    // 6. Continue active Crop Tool drag (Feature #13)
    if (self->m_is_dragging_crop) {
        if (!is_mouse_msg) return false;
        if (msg == WM_MOUSEMOVE) {
            self->m_cursor_in_client = hit.in_client;
            if (self->m_last_img_w > 0 && self->m_last_img_h > 0) {
                float doc_x = 0.0f, doc_y = 0.0f;
                self->screen_to_doc_px(static_cast<float>(pt.x), static_cast<float>(pt.y), doc_x, doc_y);
                const float nx = doc_x / static_cast<float>(self->m_last_img_w);
                const float ny = doc_y / static_cast<float>(self->m_last_img_h);
                self->m_crop_state.drag_handle(
                    self->m_crop_state.active_handle,
                    nx - self->m_crop_drag_last_norm.x,
                    ny - self->m_crop_drag_last_norm.y,
                    self->m_last_img_w, self->m_last_img_h);
                self->m_crop_drag_last_norm = ImVec2(nx, ny);
            }
            self->apply_cursor_for_state(hit);
            return true;
        }
        if (msg == WM_LBUTTONUP) {
            self->m_is_dragging_crop = false;
            self->m_crop_state.active_handle = -1;
            self->end_capture();
            self->apply_cursor_for_state(hit);
            return true;
        }
        return true;
    }

    if (self->is_viewport_capturing()) {
        if (!is_mouse_msg) return false;
        if (msg == WM_MOUSEMOVE) {
            self->m_cursor_in_client = hit.in_client;
        }
        self->vp_handle_input(msg, wParam, lParam);
        if (!self->is_viewport_capturing()) {
            self->end_capture();
        }
        self->apply_cursor_for_state(hit);
        return true;
    }

    if (msg == WM_LBUTTONDBLCLK) {
        if (hit.near_left_splitter) {
            self->m_left_panel_width = 280.0f;
            return true;
        }
        if (hit.near_right_splitter) {
            self->m_right_panel_width = 340.0f;
            return true;
        }
    }

    if (msg == WM_LBUTTONDOWN && !hit.popup_open) {
        if (hit.near_left_splitter) {
            self->begin_capture();
            self->m_resizing_left = true;
            self->apply_cursor_for_state(hit);
            return true;
        }
        if (hit.near_right_splitter) {
            self->begin_capture();
            self->m_resizing_right = true;
            self->apply_cursor_for_state(hit);
            return true;
        }
        if (hit.near_split_line) {
            self->begin_capture();
            self->m_is_dragging_split = true;
            self->apply_cursor_for_state(hit);
            return true;
        }
    }

    const bool space_active = self->m_space_held || ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0);
    self->vp_set_space_pan(space_active);

    if (hit.in_canvas && self->has_loaded_image() && !hit.popup_open) {
        // AI Object Erase (J): Left-click adds mask, Right-click subtracts mask (Feature #7)
        const bool is_left_down = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK);
        const bool is_right_down = (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK);

        if ((is_left_down || is_right_down) &&
            self->m_erase_active && self->m_active_tool == ActiveTool::AiInpaint && !space_active) {
            if (ImGui::GetIO().WantTextInput) ImGui::SetWindowFocus(nullptr);
            float uv_x = 0.0f, uv_y = 0.0f, rad_uv_x = 0.01f, rad_uv_y = 0.01f;
            bool over_image = false;
            compute_brush_uv(uv_x, uv_y, rad_uv_x, rad_uv_y, over_image);
            if (over_image) {
                self->m_is_right_click_subtract = is_right_down;
                const bool is_sub = self->m_erase_subtract || is_right_down ||
                                    ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
                self->begin_capture();
                self->m_is_painting_mask = true;
                self->push_erase_undo();
                if (is_sub) {
                    self->subtract_brush_dab(uv_x, uv_y, rad_uv_x, rad_uv_y);
                } else {
                    self->m_current_stroke.clear();
                    self->m_current_stroke.push_back(ImVec2(uv_x, uv_y));
                }
                if (self->m_last_img_w > 0 && self->m_last_img_h > 0) {
                    EraseTool &tool = EraseTool::get();
                    tool.ensure_mask_size(self->m_last_img_w, self->m_last_img_h);
                    tool.push_undo();
                    tool.brush_radius = self->m_erase_radius;
                    const float px = uv_x * static_cast<float>(self->m_last_img_w);
                    const float py = uv_y * static_cast<float>(self->m_last_img_h);
                    self->m_last_paint_px = ImVec2(px, py);
                    tool.draw_brush_dab(static_cast<int>(std::round(px)), static_cast<int>(std::round(py)),
                                        static_cast<int>(std::round(self->m_erase_radius)), is_sub ? 0 : 255);
                    if (self->m_device) {
                        tool.update_gpu_texture(self->m_device);
                    }
                }
                self->apply_cursor_for_state(hit);
                return true;
            }
            return true;
        }

        // Rotate Tool (R): Left-click drags Camera Raw horizon straighten line (Feature #6)
        if (is_left_down && self->m_active_tool == ActiveTool::Rotate && !space_active) {
            if (ImGui::GetIO().WantTextInput) ImGui::SetWindowFocus(nullptr);
            self->begin_capture();
            self->m_horizon_state.is_drawing_line = true;
            self->m_horizon_state.start_screen = ImVec2(static_cast<float>(pt.x), static_cast<float>(pt.y));
            self->m_horizon_state.end_screen = self->m_horizon_state.start_screen;
            self->m_horizon_state.preview_delta_deg = 0.0f;
            self->apply_cursor_for_state(hit);
            return true;
        }

        // Move Tool (V): Left-click drags active layer inside frame (Feature #8)
        if (is_left_down && self->m_active_tool == ActiveTool::Move && !space_active) {
            if (ImGui::GetIO().WantTextInput) ImGui::SetWindowFocus(nullptr);
            if (!self->m_active_layer_view.lock_position) {
                self->begin_capture();
                self->m_is_dragging_move = true;
                self->m_move_drag_last_screen = ImVec2(static_cast<float>(pt.x), static_cast<float>(pt.y));
                self->m_move_drag_accum = ImVec2(0.0f, 0.0f);
                self->m_move_locked_axis = 0;
            }
            self->apply_cursor_for_state(hit);
            return true;
        }

        // Raster Alpha Eraser (E): Left-click begins alpha erase stroke (Feature #12)
        if (is_left_down && self->m_active_tool == ActiveTool::RasterErase && !space_active) {
            if (ImGui::GetIO().WantTextInput) ImGui::SetWindowFocus(nullptr);
            if (self->on_get_active_layer_pixels) {
                LayerPixelView pv = self->on_get_active_layer_pixels();
                if (pv.rgba_pixels && pv.width > 0 && pv.height > 0 &&
                    !pv.lock_pixels && !pv.lock_alpha && !pv.is_text_layer) {
                    float doc_x = 0.0f, doc_y = 0.0f;
                    self->screen_to_doc_px(static_cast<float>(pt.x), static_cast<float>(pt.y), doc_x, doc_y);
                    const ImVec2 local_pt(doc_x - pv.offset_x, doc_y - pv.offset_y);
                    self->begin_capture();
                    self->m_is_raster_erasing = true;
                    self->m_last_raster_erase_px = local_pt;
                    self->m_raster_eraser.begin_stroke(*pv.rgba_pixels, pv.width, pv.height);
                    RasterEraser::DirtyRect dirty{};
                    self->m_raster_eraser.paint_segment(*pv.rgba_pixels, local_pt, local_pt, &dirty);
                    if (dirty.valid && self->on_commit_layer_pixels) {
                        self->on_commit_layer_pixels(dirty.x0, dirty.y0, dirty.x1 - dirty.x0 + 1, dirty.y1 - dirty.y0 + 1);
                    }
                }
            }
            self->apply_cursor_for_state(hit);
            return true;
        }

        // Crop Tool (C): Left-click drags handles or inside marquee (Feature #13)
        if (is_left_down && self->m_active_tool == ActiveTool::Crop && !space_active) {
            if (ImGui::GetIO().WantTextInput) ImGui::SetWindowFocus(nullptr);
            const int handle = self->hit_test_crop_handle(static_cast<float>(pt.x), static_cast<float>(pt.y));
            if (handle >= 0 && self->m_last_img_w > 0 && self->m_last_img_h > 0) {
                float doc_x = 0.0f, doc_y = 0.0f;
                self->screen_to_doc_px(static_cast<float>(pt.x), static_cast<float>(pt.y), doc_x, doc_y);
                self->begin_capture();
                self->m_crop_state.active_handle = handle;
                self->m_is_dragging_crop = true;
                self->m_crop_drag_last_norm = ImVec2(doc_x / static_cast<float>(self->m_last_img_w),
                                                     doc_y / static_cast<float>(self->m_last_img_h));
            }
            self->apply_cursor_for_state(hit);
            return true;
        }

        // Text Tool (T): Left-click places or selects text layer (Feature #15)
        if (is_left_down && self->m_active_tool == ActiveTool::Text && !space_active) {
            if (ImGui::GetIO().WantTextInput) ImGui::SetWindowFocus(nullptr);
            float doc_x = 0.0f, doc_y = 0.0f;
            self->screen_to_doc_px(static_cast<float>(pt.x), static_cast<float>(pt.y), doc_x, doc_y);
            if (self->on_text_canvas_click) {
                self->on_text_canvas_click(doc_x, doc_y);
                self->mark_active_document_dirty();
            }
            self->apply_cursor_for_state(hit);
            return true;
        }

        if (msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_RBUTTONDOWN ||
            msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK ||
            msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
            if (ImGui::GetIO().WantTextInput) {
                ImGui::SetWindowFocus(nullptr);
            }
            const bool is_down = (msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_RBUTTONDOWN);
            if (is_down) {
                self->begin_capture();
            }
            self->vp_handle_input(msg, wParam, lParam);
            if (!self->is_viewport_capturing()) {
                self->end_capture();
            }
            self->apply_cursor_for_state(hit);
            return true;
        }

        if (msg == WM_MOUSEMOVE) {
            self->m_cursor_in_client = hit.in_client;
            ImGui_ImplWin32_WndProcHandler(self->m_hwnd, msg, wParam, lParam);
            self->apply_cursor_for_state(hit);
            return false;
        }
    }

    const bool imgui_handled = ImGui_ImplWin32_WndProcHandler(self->m_hwnd, msg, wParam, lParam) != 0;
    if (msg == WM_MOUSEMOVE) {
        self->m_cursor_in_client = hit.in_client;
        self->apply_cursor_for_state(hit);
    }
    if (imgui_handled) return true;

    const ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse &&
        ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)) {
        return true;
    }

    return false;
}

bool GuiManager::want_capture_mouse() const {
    if (!m_initialized) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

bool GuiManager::want_capture_keyboard() const {
    if (!m_initialized) return false;
    return ImGui::GetIO().WantCaptureKeyboard;
}

float GuiManager::get_left_panel_width(uint32_t screen_w) const {
    if (m_hide_panels) return 0.0f;
    return (std::min)(m_left_panel_width, static_cast<float>(screen_w) * 0.45f);
}

float GuiManager::get_right_panel_width(uint32_t screen_w) const {
    if (m_hide_panels) return 0.0f;
    return (std::min)(m_right_panel_width, static_cast<float>(screen_w) * 0.45f);
}

float GuiManager::get_menu_bar_height() const {
    return ImGui::GetFrameHeight();
}

void GuiManager::begin_frame() {
    if (!m_initialized) return;

    m_dom.clear();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    if (m_sim_mouse_down_prev) {
        ImGuiIO &io = ImGui::GetIO();
        io.AddMouseButtonEvent(0, false);
        m_sim_mouse_down_prev = false;
    }

    while (!m_sim_mouse_queue.empty()) {
        const auto ev = m_sim_mouse_queue.front();
        m_sim_mouse_queue.erase(m_sim_mouse_queue.begin());
        ImGuiIO &io = ImGui::GetIO();
        m_last_mouse_pt = { static_cast<LONG>(ev.x), static_cast<LONG>(ev.y) };
        m_cursor_in_client = true;
        io.AddMousePosEvent(ev.x, ev.y);
        if (ev.action == 1) {
            io.AddMouseButtonEvent(0, true);
            m_sim_mouse_down_prev = true;
        } else if (ev.action == 0) {
            io.AddMouseButtonEvent(0, false);
        }
    }

    ImGui::NewFrame();
}

void GuiManager::render_tool_options_bar(float x_offset, float top_offset, float width, float height,
                                         uint32_t img_w, uint32_t img_h) {
    ImGui::SetNextWindowPos(ImVec2(x_offset, top_offset));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;

    if (ImGui::Begin("##ToolOptionsContextBar", nullptr, kFlags)) {
        ImGui::AlignTextToFramePadding();

        switch (m_active_tool) {
        case ActiveTool::Move: {
            ImGui::TextColored(ImVec4(0.45f, 0.88f, 0.94f, 1.0f), "Move (V)");
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::TextUnformatted("X:");
            ImGui::SameLine(0.0f, 4.0f);

            float ox = m_active_layer_view.offset_x;
            float oy = m_active_layer_view.offset_y;
            ImGui::SetNextItemWidth(76.0f);
            ImGui::BeginDisabled(m_active_layer_view.lock_position);
            if (ImGui::DragFloat("##move_x", &ox, 1.0f, -8192.0f, 8192.0f, "%.1f px")) {
                m_active_layer_view.offset_x = ox;
                m_has_pending_abs_offset = true;
                m_pending_abs_offset_x = ox;
                m_pending_abs_offset_y = oy;
                if (on_set_layer_offset) on_set_layer_offset(ox, oy);
                mark_active_document_dirty();
            }
            register_dom_last_item("move_input_x", "X Offset", "slider", false, !m_active_layer_view.lock_position);

            ImGui::SameLine();
            ImGui::TextUnformatted("Y:");
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::SetNextItemWidth(76.0f);
            if (ImGui::DragFloat("##move_y", &oy, 1.0f, -8192.0f, 8192.0f, "%.1f px")) {
                m_active_layer_view.offset_y = oy;
                m_has_pending_abs_offset = true;
                m_pending_abs_offset_x = ox;
                m_pending_abs_offset_y = oy;
                if (on_set_layer_offset) on_set_layer_offset(ox, oy);
                mark_active_document_dirty();
            }
            register_dom_last_item("move_input_y", "Y Offset", "slider", false, !m_active_layer_view.lock_position);

            ImGui::SameLine();
            if (ImGui::SmallButton("Center Layer")) {
                m_active_layer_view.offset_x = 0.0f;
                m_active_layer_view.offset_y = 0.0f;
                m_req_center_layer = true;
                if (on_center_layer) on_center_layer();
                mark_active_document_dirty();
            }
            register_dom_last_item("move_btn_center", "Center Layer", "button", false, !m_active_layer_view.lock_position);
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::Checkbox("Show Layer Bounds", &m_show_layer_bounds);
            register_dom_last_item("move_chk_bounds", "Show Layer Bounds", "checkbox", m_show_layer_bounds, true);

            if (m_active_layer_view.lock_position) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "(Position Locked)");
            }
            break;
        }

        case ActiveTool::Rotate: {
            ImGui::TextColored(ImVec4(0.95f, 0.84f, 0.35f, 1.0f), "Rotate (R)");
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::TextUnformatted("Angle:");
            ImGui::SameLine(0.0f, 4.0f);

            const float base_quad = std::round(HorizonAlignState::normalize_angle_signed180(m_last_angle) / 90.0f) * 90.0f;
            float fine_angle = HorizonAlignState::normalize_angle_signed180(m_last_angle - base_quad);
            ImGui::SetNextItemWidth(135.0f);
            if (ImGui::SliderFloat("##rot_angle", &fine_angle, -45.0f, 45.0f, "%.2f deg")) {
                apply_rotation_delta(base_quad + fine_angle);
            }
            m_is_dragging_angle_slider = ImGui::IsItemActive();
            register_dom_last_item("rot_slider_angle", "Angle", "slider", m_is_dragging_angle_slider, true);

            ImGui::SameLine();
            if (ImGui::SmallButton("90 CCW")) {
                apply_rotation_delta(m_last_angle - 90.0f);
            }
            register_dom_last_item("rot_btn_ccw90", "90 CCW", "button", false, true);

            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::SmallButton("90 CW")) {
                apply_rotation_delta(m_last_angle + 90.0f);
            }
            register_dom_last_item("rot_btn_cw90", "90 CW", "button", false, true);

            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::SmallButton("Flip H")) {
                m_horizon_state.flip_horizontal = !m_horizon_state.flip_horizontal;
                m_req_flip_horizontal = true;
                if (on_flip_layer) on_flip_layer(true, false);
                mark_active_document_dirty();
            }
            register_dom_last_item("rot_btn_flip_h", "Flip H", "button", m_horizon_state.flip_horizontal, true);

            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::SmallButton("Flip V")) {
                m_horizon_state.flip_vertical = !m_horizon_state.flip_vertical;
                m_req_flip_vertical = true;
                if (on_flip_layer) on_flip_layer(false, true);
                mark_active_document_dirty();
            }
            register_dom_last_item("rot_btn_flip_v", "Flip V", "button", m_horizon_state.flip_vertical, true);

            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::SmallButton("Reset")) {
                m_horizon_state.flip_horizontal = false;
                m_horizon_state.flip_vertical = false;
                apply_rotation_delta(0.0f);
            }
            register_dom_last_item("rot_btn_reset", "Reset", "button", false, true);

            ImGui::SameLine();
            if (ImGui::Checkbox("Constrain to Image", &m_horizon_state.auto_scale_to_fit)) {
                m_req_set_viewport_angle = true;
                m_target_viewport_angle = m_last_angle;
                if (on_rotate_apply) on_rotate_apply(m_last_angle, m_horizon_state.auto_scale_to_fit);
            }
            register_dom_last_item("rot_chk_constrain", "Constrain to Image", "checkbox", m_horizon_state.auto_scale_to_fit, true);
            break;
        }

        case ActiveTool::Crop: {
            ImGui::TextColored(ImVec4(0.85f, 0.90f, 0.96f, 1.0f), "Crop (C)");
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::TextUnformatted("Preset:");
            ImGui::SameLine(0.0f, 4.0f);

            const char *preset_names[CropToolState::kPresetCount] = {
                "Freeform (Unconstrained)",
                "Original Ratio",
                "1:1 (Square)",
                "16:9 (Widescreen)",
                "9:16 (Vertical / Reel)",
                "4:3 (Standard Photo)",
                "3:2 (35mm DSLR)",
                "4:5 (Portrait Social)",
                "21:9 (Ultrawide Cinema)"
            };
            int preset_idx = static_cast<int>(m_crop_state.preset);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("##crop_preset", &preset_idx, preset_names, CropToolState::kPresetCount)) {
                m_crop_state.preset = static_cast<CropToolState::Preset>(preset_idx);
                m_crop_state.aspect_swapped = false;
                m_crop_state.apply_preset_ratio(img_w, img_h);
            }
            register_dom_last_item("crop_combo_preset", CropToolState::get_preset_name(m_crop_state.preset), "combo", false, true);

            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::SmallButton("Swap W/H")) {
                m_crop_state.swap_orientation(img_w, img_h);
            }
            register_dom_last_item("crop_btn_swap", "Swap W/H", "button", m_crop_state.aspect_swapped, true);

            ImGui::SameLine();
            const auto prect = m_crop_state.get_pixel_rect(img_w, img_h);
            ImGui::Text("%dx%d", prect.w, prect.h);

            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) {
                m_crop_state.reset(img_w, img_h);
            }
            register_dom_last_item("crop_btn_reset", "Reset", "button", false, true);

            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::SmallButton("Cancel (Esc)")) {
                cancel_crop_tool();
            }
            register_dom_last_item("crop_btn_cancel", "Cancel (Esc)", "button", false, true);

            ImGui::SameLine(0.0f, 4.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.50f, 0.30f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.62f, 0.38f, 1.00f));
            if (ImGui::SmallButton("Apply Crop (Enter)")) {
                trigger_apply_crop();
            }
            ImGui::PopStyleColor(2);
            register_dom_last_item("crop_btn_apply", "Apply Crop (Enter)", "button", false, img_w > 0 && img_h > 0);
            break;
        }

        case ActiveTool::RasterErase: {
            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.45f, 1.0f), "Eraser (E)");
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            ImGui::TextUnformatted("Size:");
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("##eraser_size", &m_raster_eraser.brush_size, 1.0f, 500.0f, "%.0f px");
            register_dom_last_item("eraser_slider_size", "Brush Size", "slider", ImGui::IsItemActive(), true);

            ImGui::SameLine();
            ImGui::TextUnformatted("Hardness:");
            ImGui::SameLine(0.0f, 4.0f);
            float hard_pct = m_raster_eraser.brush_hardness * 100.0f;
            ImGui::SetNextItemWidth(95.0f);
            if (ImGui::SliderFloat("##eraser_hard", &hard_pct, 0.0f, 100.0f, "%.0f%%")) {
                m_raster_eraser.brush_hardness = std::clamp(hard_pct * 0.01f, 0.0f, 1.0f);
            }
            register_dom_last_item("eraser_slider_hardness", "Hardness", "slider", ImGui::IsItemActive(), true);

            ImGui::SameLine();
            ImGui::TextUnformatted("Opacity:");
            ImGui::SameLine(0.0f, 4.0f);
            float opac_pct = m_raster_eraser.brush_opacity * 100.0f;
            ImGui::SetNextItemWidth(95.0f);
            if (ImGui::SliderFloat("##eraser_opac", &opac_pct, 1.0f, 100.0f, "%.0f%%")) {
                m_raster_eraser.brush_opacity = std::clamp(opac_pct * 0.01f, 0.01f, 1.0f);
            }
            register_dom_last_item("eraser_slider_opacity", "Opacity", "slider", ImGui::IsItemActive(), true);

            if (m_active_layer_view.lock_pixels || m_active_layer_view.lock_alpha) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "(Layer Alpha/Pixels Locked)");
            }
            break;
        }

        case ActiveTool::AiInpaint: {
            ImGui::TextColored(ImVec4(0.95f, 0.50f, 0.50f, 1.0f), "AI Object Erase (J)");
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            ImGui::TextUnformatted("Radius:");
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::SliderFloat("##inpaint_radius", &m_erase_radius, 2.0f, 256.0f, "%.0f px")) {
                EraseTool::get().brush_radius = m_erase_radius;
            }
            register_dom_last_item("erase_slider_radius", "Brush Radius", "slider", ImGui::IsItemActive(), true);

            ImGui::SameLine();
            ImGui::Checkbox("Subtract (-)", &m_erase_subtract);
            register_dom_last_item("erase_checkbox_subtract", "Subtract", "checkbox", m_erase_subtract, true);

            ImGui::SameLine();
            ImGui::Checkbox("Auto-apply", &EraseTool::get().auto_apply);
            register_dom_last_item("erase_checkbox_auto_apply", "Auto-apply", "checkbox", EraseTool::get().auto_apply, true);

            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                clear_erase_strokes();
                m_req_clear_mask = true;
            }
            register_dom_last_item("erase_btn_clear", "Clear", "button", false, true);

            ImGui::SameLine(0.0f, 4.0f);
            const bool can_inpaint = EraseTool::get().has_mask_edits() && !EraseTool::get().is_processing();
            ImGui::BeginDisabled(!can_inpaint);
            if (ImGui::SmallButton("Erase Object")) {
                request_inpaint();
            }
            ImGui::EndDisabled();
            register_dom_last_item("erase_btn_inpaint", "Erase Object", "button", false, can_inpaint);
            break;
        }

        case ActiveTool::Text:
            ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.0f), "Text Tool (T)");
            ImGui::SameLine();
            ImGui::TextDisabled("| Click anywhere on the canvas to place or edit a DirectWrite text layer.");
            break;

        case ActiveTool::Upscale:
            ImGui::TextColored(ImVec4(0.60f, 0.92f, 0.72f, 1.0f), "AI Upscaler (U)");
            ImGui::SameLine();
            ImGui::TextDisabled("| Choose Real-ESRGAN / RealPLKSR model and factor (2x / 4x) in the Upscale tab.");
            break;

        case ActiveTool::Depth:
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f), "3D Depth (D)");
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::Checkbox("Invert Depth", &m_depth_invert);
            register_dom_last_item("depth_chk_invert", "Invert Depth", "checkbox", m_depth_invert, true);
            break;

        case ActiveTool::Split:
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f), "Split Compare (B)");
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            if (ImGui::SmallButton("Center Divider")) {
                m_before_after_split = 0.0f;
            }
            register_dom_last_item("split_btn_center", "Center Divider", "button", false, true);
            ImGui::SameLine();
            ImGui::TextDisabled("Drag the vertical amber bar on the canvas.");
            break;

        case ActiveTool::Hand:
        default:
            ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.90f, 1.0f), "Hand Tool (H)");
            ImGui::SameLine();
            ImGui::TextDisabled("| Drag to pan, scroll to zoom");
            ImGui::SameLine();
            if (ImGui::SmallButton("Fit")) m_req_fit_view = true;
            register_dom_last_item("opt_fit", "Fit", "button", false, true);
            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::SmallButton("100%")) m_req_100_view = true;
            register_dom_last_item("opt_100", "100%", "button", false, true);
            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::SmallButton("200%")) m_req_200_view = true;
            register_dom_last_item("opt_200", "200%", "button", false, true);
            ImGui::SameLine();
            ImGui::Checkbox("Static Zoom", &m_static_zoom);
            register_dom_last_item("opt_static_zoom", "Static Zoom", "checkbox", m_static_zoom, true);
            break;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void GuiManager::render(uint32_t screen_w, uint32_t screen_h,
                        uint32_t img_w, uint32_t img_h,
                        float zoom_level, float pan_x, float pan_y) {
    if (!m_initialized) return;

    const float menu_height = get_menu_bar_height();
    const float tab_bar_h = get_tab_bar_height();
    const float context_bar_h = get_context_bar_height();
    const float tool_options_h = get_tool_options_height();
    const float toolbar_w = get_toolbar_width();
    const float status_bar_h = get_status_bar_height();

    const float left_w = get_left_panel_width(screen_w);
    const float right_w = get_right_panel_width(screen_w);
    const float total_w = static_cast<float>(screen_w);
    const float total_h = static_cast<float>(screen_h);

    const float full_body_y = menu_height;
    const float full_body_h = (std::max)(64.0f, total_h - full_body_y - status_bar_h);

    const float viewable_x = toolbar_w + left_w;
    const float viewable_w = (std::max)(64.0f, total_w - viewable_x - right_w);

    const float canvas_y = menu_height + tool_options_h;
    const float canvas_h = (std::max)(64.0f, total_h - canvas_y - status_bar_h);

    m_canvas_view_x = viewable_x;
    m_canvas_view_y = canvas_y;
    m_canvas_view_w = viewable_w;
    m_canvas_view_h = canvas_h;
    m_last_img_w = img_w;
    m_last_img_h = img_h;
    m_last_zoom = zoom_level;
    m_last_pan_x = pan_x;
    m_last_pan_y = pan_y;

    render_tool_strip(*this, full_body_y, full_body_h);

    if (!m_hide_panels && left_w > 0.0f) {
        render_left_panel(*this, toolbar_w, full_body_y, left_w, full_body_h);
    }

    render_tab_bar(*this, viewable_x, menu_height, viewable_w);
    render_tool_options_bar(viewable_x, menu_height + tab_bar_h, viewable_w, context_bar_h, img_w, img_h);

    render_center_canvas(*this, viewable_x, canvas_y, viewable_w, canvas_h, img_w, img_h, zoom_level, pan_x, pan_y);

    if (!m_hide_panels && right_w > 0.0f) {
        render_right_panel(*this, total_w - right_w, full_body_y, right_w, full_body_h);
    }

    render_bottom_status_bar(*this, total_h - status_bar_h, total_w, status_bar_h, img_w, img_h, zoom_level, pan_x, pan_y);

    render_menu_bar(*this);

    if (render_exit_modal_fn) {
        render_exit_modal_fn();
    }

    if (m_show_about_dialog) {
        ImGui::OpenPopup("About ShaderLab");
        m_show_about_dialog = false;
    }

    if (ImGui::BeginPopupModal("About ShaderLab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(280.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "ShaderLab Studio v2.0");
        ImGui::TextDisabled("Realtime Shader, Layer & Depth Compositing Studio");
        ImGui::Separator();
        ImGui::TextWrapped("High-performance D3D11 layer & shader workbench for ReShade FX, HLSL, Unity, and Godot shaders with neural inpainting, upscaling, and depth estimation.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(-1.0f, 26.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (m_show_shortcuts_dialog) {
        ImGui::OpenPopup("Keyboard Shortcuts");
        m_show_shortcuts_dialog = false;
    }

    if (ImGui::BeginPopupModal("Keyboard Shortcuts", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(380.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "Keyboard Shortcuts Reference");
        ImGui::Separator();
        ImGui::TextUnformatted("Tools:");
        ImGui::BulletText("V: Move Layer Tool");
        ImGui::BulletText("H: Hand / Pan Tool");
        ImGui::BulletText("R: Rotate & Horizon Align Tool");
        ImGui::BulletText("C: Crop Tool");
        ImGui::BulletText("E: Raster Alpha Eraser Tool");
        ImGui::BulletText("J: AI Object Erase (LaMa)");
        ImGui::BulletText("T: System Font Text Tool");
        ImGui::BulletText("U: AI Upscaler Tool");
        ImGui::BulletText("D: 3D Depth Peek (Hold)");
        ImGui::BulletText("B: Split Before / After Compare");
        ImGui::BulletText("\\: Master Bypass");
        ImGui::BulletText("Tab: Show / Hide Side Panels");
        ImGui::Spacing();
        ImGui::TextUnformatted("Project & View:");
        ImGui::BulletText("Ctrl + O: Open Image / Project");
        ImGui::BulletText("Ctrl + S / Ctrl+Shift+S: Save / Save As (.shaderlab)");
        ImGui::BulletText("Ctrl + E / Ctrl+Shift+E: Export / Export As Image");
        ImGui::BulletText("Ctrl + ,: Preferences");
        ImGui::BulletText("Space + Drag: Hand Pan");
        ImGui::BulletText("Ctrl + 0 / 1 / 2: Fit / 100% / 200%");
        ImGui::BulletText("[ / ]: Decrease / Increase Brush Size");
        ImGui::BulletText("Right-Click / Alt: Subtract AI Inpaint Mask");
        ImGui::BulletText("Enter: Apply Crop / Run AI Inpaint");
        ImGui::BulletText("Esc: Cancel Crop / Clear Mask / Hand Tool");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(-1.0f, 26.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void GuiManager::end_frame() {
    if (!m_initialized) return;
    if (m_cursor_in_client || has_active_drag()) {
        apply_cursor_for_state(compute_cursor_hit_state(m_last_mouse_pt));
    }
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

std::string GuiManager::get_dom_json() const {
    std::string json = m_dom.to_json();
    const size_t close_pos = json.rfind('}');
    if (close_pos == std::string::npos) {
        return json;
    }
    size_t prev = close_pos;
    while (prev > 0 && (json[prev - 1] == ' ' || json[prev - 1] == '\t' ||
                        json[prev - 1] == '\r' || json[prev - 1] == '\n')) {
        --prev;
    }
    const bool empty_obj = (prev > 0 && json[prev - 1] == '{');
    std::string field = empty_obj ? "" : ", ";
    field += "\"active_cursor\": \"" + m_active_cursor_name + "\", ";
    field += "\"active_tool\": \"" + std::string(active_tool_to_string(m_active_tool)) + "\", ";
    field += "\"mask_white_px\": " + std::to_string(EraseTool::get().get_mask_white_px()) + ", ";
    field += "\"erase_step\": " + std::to_string(EraseTool::get().get_current_erase_step()) + ", ";
    field += "\"erase_total\": " + std::to_string(EraseTool::get().get_total_erase_steps()) + ", ";
    field += "\"active_tab\": " + std::to_string(m_active_tab) + ", ";
    field += "\"tab_count\": " + std::to_string(static_cast<int>(m_tabs.size())) + ", ";
    field += "\"viewport_angle\": " + std::to_string(m_last_angle) + ", ";
    field += "\"layer_offset_x\": " + std::to_string(m_active_layer_view.offset_x) + ", ";
    field += "\"layer_offset_y\": " + std::to_string(m_active_layer_view.offset_y) + ", ";
    field += "\"crop_x0\": " + std::to_string(m_crop_state.x0) + ", ";
    field += "\"crop_y0\": " + std::to_string(m_crop_state.y0) + ", ";
    field += "\"crop_x1\": " + std::to_string(m_crop_state.x1) + ", ";
    field += "\"crop_y1\": " + std::to_string(m_crop_state.y1);
    json.insert(close_pos, field);
    return json;
}

void GuiManager::simulate_canvas_mouse(float x, float y, int act) {
    if (m_last_img_w == 0 || m_last_img_h == 0) {
        return;
    }
    const bool in_canvas = (x >= m_canvas_view_x && x < (m_canvas_view_x + m_canvas_view_w) &&
                            y >= m_canvas_view_y && y < (m_canvas_view_y + m_canvas_view_h));
    if (!in_canvas && (act == 1 || act == 2 || act == 4 || !has_active_drag())) {
        return;
    }

    if (m_active_tool == ActiveTool::AiInpaint && m_erase_active) {
        float img_px_x = 0.0f, img_px_y = 0.0f;
        screen_to_doc_px(x, y, img_px_x, img_px_y);
        const float clamped_px_x = std::clamp(img_px_x, 0.0f, static_cast<float>(m_last_img_w));
        const float clamped_px_y = std::clamp(img_px_y, 0.0f, static_cast<float>(m_last_img_h));
        const float uv_x = clamped_px_x / static_cast<float>(m_last_img_w);
        const float uv_y = clamped_px_y / static_cast<float>(m_last_img_h);
        const float rad_uv_x = m_erase_radius / static_cast<float>(m_last_img_w);
        const float rad_uv_y = m_erase_radius / static_cast<float>(m_last_img_h);

        if (act == 4) m_is_right_click_subtract = true;
        else if (act == 2) m_is_right_click_subtract = false;

        const bool is_sub = m_erase_subtract || m_is_right_click_subtract || (act == 4 || act == 5 || act == 6);

        EraseTool &tool = EraseTool::get();
        tool.ensure_mask_size(m_last_img_w, m_last_img_h);
        tool.brush_radius = m_erase_radius;
        const ImVec2 curr_px(clamped_px_x, clamped_px_y);

        bool finished_non_sub_stroke = false;
        if (act == 2 || act == 4) { // Left Down (2) or Right Down (4)
            m_is_painting_mask = true;
            push_erase_undo();
            tool.push_undo();
            m_last_paint_px = curr_px;
            tool.draw_brush_dab(static_cast<int>(std::round(curr_px.x)), static_cast<int>(std::round(curr_px.y)),
                                static_cast<int>(std::round(m_erase_radius)), is_sub ? 0 : 255);
            if (is_sub) {
                subtract_brush_dab(uv_x, uv_y, rad_uv_x, rad_uv_y);
            } else {
                m_current_stroke.clear();
                m_current_stroke.push_back(ImVec2(uv_x, uv_y));
            }
        } else if (act == 1 || act == 6) { // Left Click (1) or Right Click (6)
            m_is_painting_mask = false;
            m_is_right_click_subtract = false;
            m_last_paint_px = ImVec2(-1.0f, -1.0f);
            push_erase_undo();
            tool.push_undo();
            tool.draw_brush_dab(static_cast<int>(std::round(curr_px.x)), static_cast<int>(std::round(curr_px.y)),
                                static_cast<int>(std::round(m_erase_radius)), is_sub ? 0 : 255);
            if (is_sub) {
                subtract_brush_dab(uv_x, uv_y, rad_uv_x, rad_uv_y);
            } else {
                m_erase_strokes.push_back({ ImVec2(uv_x, uv_y) });
                finished_non_sub_stroke = true;
            }
        } else if (act == 0 && m_is_painting_mask) { // Move
            if (m_last_paint_px.x >= 0.0f && m_last_paint_px.y >= 0.0f) {
                tool.draw_brush_line(m_last_paint_px, curr_px, m_erase_radius, is_sub);
            } else {
                tool.draw_brush_dab(static_cast<int>(std::round(curr_px.x)), static_cast<int>(std::round(curr_px.y)),
                                    static_cast<int>(std::round(m_erase_radius)), is_sub ? 0 : 255);
            }
            m_last_paint_px = curr_px;
            if (is_sub) {
                subtract_brush_dab(uv_x, uv_y, rad_uv_x, rad_uv_y);
            } else {
                m_current_stroke.push_back(ImVec2(uv_x, uv_y));
            }
        } else if ((act == 3 || act == 5) && m_is_painting_mask) { // Left Up (3) or Right Up (5)
            const bool was_sub = is_sub || (act == 5);
            m_is_painting_mask = false;
            m_is_right_click_subtract = false;
            m_last_paint_px = ImVec2(-1.0f, -1.0f);
            if (!was_sub && !m_current_stroke.empty()) {
                m_erase_strokes.push_back(m_current_stroke);
                m_current_stroke.clear();
            } else {
                m_current_stroke.clear();
            }
            if (!was_sub) finished_non_sub_stroke = true;
        }
        if (m_device) {
            tool.update_gpu_texture(m_device);
        }
        if (finished_non_sub_stroke) {
            on_erase_stroke_finished(false);
        }
        return;
    }

    if (m_active_tool == ActiveTool::Rotate) {
        if (act == 2) {
            m_horizon_state.is_drawing_line = true;
            m_horizon_state.start_screen = ImVec2(x, y);
            m_horizon_state.end_screen = ImVec2(x, y);
            m_horizon_state.preview_delta_deg = 0.0f;
        } else if (act == 0 && m_horizon_state.is_drawing_line) {
            m_horizon_state.end_screen = ImVec2(x, y);
            m_horizon_state.preview_delta_deg = HorizonAlignState::compute_horizon_straighten_angle(
                m_horizon_state.start_screen, m_horizon_state.end_screen);
        } else if (act == 3 && m_horizon_state.is_drawing_line) {
            m_horizon_state.end_screen = ImVec2(x, y);
            m_horizon_state.is_drawing_line = false;
            const float delta = HorizonAlignState::compute_horizon_straighten_angle(
                m_horizon_state.start_screen, m_horizon_state.end_screen);
            if (std::abs(delta) > 0.001f) {
                apply_rotation_delta(m_last_angle + delta);
            }
        }
        return;
    }

    if (m_active_tool == ActiveTool::Move && !m_active_layer_view.lock_position) {
        if (act == 2) {
            m_is_dragging_move = true;
            m_move_drag_last_screen = ImVec2(x, y);
            m_move_drag_accum = ImVec2(0.0f, 0.0f);
            m_move_locked_axis = 0;
        } else if (act == 0 && m_is_dragging_move) {
            float cx = 0.0f, cy = 0.0f, eff_zoom = 1.0f, cos_a = 1.0f, sin_a = 0.0f;
            compute_viewport_metrics(m_canvas_view_x, m_canvas_view_y, m_canvas_view_w, m_canvas_view_h,
                                     m_last_img_w, m_last_img_h, m_last_zoom, m_last_pan_x, m_last_pan_y,
                                     m_last_angle, cx, cy, eff_zoom, cos_a, sin_a);
            const float dsx = x - m_move_drag_last_screen.x;
            const float dsy = y - m_move_drag_last_screen.y;
            m_move_drag_last_screen = ImVec2(x, y);
            const float dx = (dsx * cos_a + dsy * sin_a) / eff_zoom;
            const float dy = (-dsx * sin_a + dsy * cos_a) / eff_zoom;
            nudge_active_layer(dx, dy);
        } else if (act == 3 && m_is_dragging_move) {
            m_is_dragging_move = false;
            m_move_locked_axis = 0;
        }
        return;
    }

    if (m_active_tool == ActiveTool::RasterErase) {
        if (!on_get_active_layer_pixels) return;
        LayerPixelView pv = on_get_active_layer_pixels();
        if (!pv.rgba_pixels || pv.width == 0 || pv.height == 0 ||
            pv.lock_pixels || pv.lock_alpha || pv.is_text_layer) {
            return;
        }
        float doc_x = 0.0f, doc_y = 0.0f;
        screen_to_doc_px(x, y, doc_x, doc_y);
        const ImVec2 local_pt(doc_x - pv.offset_x, doc_y - pv.offset_y);
        if (act == 2 || act == 1) {
            m_is_raster_erasing = (act == 2);
            m_last_raster_erase_px = local_pt;
            m_raster_eraser.begin_stroke(*pv.rgba_pixels, pv.width, pv.height);
            RasterEraser::DirtyRect dirty{};
            m_raster_eraser.paint_segment(*pv.rgba_pixels, local_pt, local_pt, &dirty);
            if (dirty.valid && on_commit_layer_pixels) {
                on_commit_layer_pixels(dirty.x0, dirty.y0, dirty.x1 - dirty.x0 + 1, dirty.y1 - dirty.y0 + 1);
            }
            if (act == 1) {
                std::vector<uint8_t> before_rgba;
                if (m_raster_eraser.end_stroke(before_rgba) && on_push_layer_pixel_undo) {
                    on_push_layer_pixel_undo(before_rgba);
                }
                mark_active_document_dirty();
            }
        } else if (act == 0 && m_is_raster_erasing) {
            RasterEraser::DirtyRect dirty{};
            m_raster_eraser.paint_segment(*pv.rgba_pixels, m_last_raster_erase_px, local_pt, &dirty);
            m_last_raster_erase_px = local_pt;
            if (dirty.valid && on_commit_layer_pixels) {
                on_commit_layer_pixels(dirty.x0, dirty.y0, dirty.x1 - dirty.x0 + 1, dirty.y1 - dirty.y0 + 1);
            }
        } else if (act == 3 && m_is_raster_erasing) {
            m_is_raster_erasing = false;
            m_last_raster_erase_px = ImVec2(-1.0f, -1.0f);
            std::vector<uint8_t> before_rgba;
            if (m_raster_eraser.end_stroke(before_rgba) && on_push_layer_pixel_undo) {
                on_push_layer_pixel_undo(before_rgba);
            }
            mark_active_document_dirty();
        }
        return;
    }

    if (m_active_tool == ActiveTool::Crop) {
        float doc_x = 0.0f, doc_y = 0.0f;
        screen_to_doc_px(x, y, doc_x, doc_y);
        const float nx = doc_x / static_cast<float>(m_last_img_w);
        const float ny = doc_y / static_cast<float>(m_last_img_h);
        if (act == 2) {
            const int handle = hit_test_crop_handle(x, y);
            if (handle >= 0) {
                m_crop_state.active_handle = handle;
                m_is_dragging_crop = true;
                m_crop_drag_last_norm = ImVec2(nx, ny);
            }
        } else if (act == 0 && m_is_dragging_crop) {
            m_crop_state.drag_handle(m_crop_state.active_handle,
                                     nx - m_crop_drag_last_norm.x,
                                     ny - m_crop_drag_last_norm.y,
                                     m_last_img_w, m_last_img_h);
            m_crop_drag_last_norm = ImVec2(nx, ny);
        } else if (act == 3 && m_is_dragging_crop) {
            m_is_dragging_crop = false;
            m_crop_state.active_handle = -1;
        }
        return;
    }

    if (m_active_tool == ActiveTool::Text && (act == 1 || act == 2)) {
        float doc_x = 0.0f, doc_y = 0.0f;
        screen_to_doc_px(x, y, doc_x, doc_y);
        if (on_text_canvas_click) {
            on_text_canvas_click(doc_x, doc_y);
            mark_active_document_dirty();
        }
        return;
    }
}

bool GuiManager::click_dom_element(const std::string &id_or_label) {
    // Right panel tabs
    if (id_or_label == "tab_shaders") { select_right_panel_tab(0); return true; }
    if (id_or_label == "tab_depth" || id_or_label == "accordion_depth") { select_right_panel_tab(1); return true; }
    if (id_or_label == "tab_ai_inpaint" || id_or_label == "tab_erase" || id_or_label == "accordion_erase") {
        select_right_panel_tab(2);
        return true;
    }
    if (id_or_label == "tab_upscale") { select_right_panel_tab(3); return true; }
    if (id_or_label == "tab_text") { select_right_panel_tab(4); return true; }
    if (id_or_label == "tab_prefs") { select_right_panel_tab(5); return true; }

    // Tools
    if (id_or_label == "tool_move" || id_or_label == "tool_V" || id_or_label == "tool_v") {
        set_active_tool(ActiveTool::Move);
        return true;
    }
    if (id_or_label == "tool_hand" || id_or_label == "tool_H" || id_or_label == "tool_h") {
        set_active_tool(ActiveTool::Hand);
        return true;
    }
    if (id_or_label == "tool_rotate" || id_or_label == "tool_R" || id_or_label == "tool_r") {
        set_active_tool(ActiveTool::Rotate);
        return true;
    }
    if (id_or_label == "tool_crop" || id_or_label == "tool_C" || id_or_label == "tool_c") {
        set_active_tool(ActiveTool::Crop);
        return true;
    }
    if (id_or_label == "tool_eraser" || id_or_label == "tool_erase" ||
        id_or_label == "tool_E" || id_or_label == "tool_e") {
        set_active_tool(ActiveTool::RasterErase);
        return true;
    }
    if (id_or_label == "tool_ai_inpaint" || id_or_label == "tool_J" || id_or_label == "tool_j") {
        set_active_tool(ActiveTool::AiInpaint);
        return true;
    }
    if (id_or_label == "tool_text" || id_or_label == "tool_T" || id_or_label == "tool_t") {
        set_active_tool(ActiveTool::Text);
        return true;
    }
    if (id_or_label == "tool_upscale" || id_or_label == "tool_U" || id_or_label == "tool_u") {
        set_active_tool(ActiveTool::Upscale);
        return true;
    }
    if (id_or_label == "tool_depth" || id_or_label == "tool_D" || id_or_label == "tool_d") {
        set_active_tool(ActiveTool::Depth);
        return true;
    }
    if (id_or_label == "tool_split" || id_or_label == "tool_B" || id_or_label == "tool_b") {
        set_active_tool(ActiveTool::Split);
        return true;
    }
    if (id_or_label == "tool_bypass" || id_or_label == "tool_\\") {
        m_master_bypass = !m_master_bypass;
        return true;
    }

    // Menu & Exit modal actions (for @lifecycle-ui automation)
    if (id_or_label == "menu_file_save") { m_req_save_project = true; return true; }
    if (id_or_label == "menu_file_save_as") { m_req_save_project_as = true; return true; }
    if (id_or_label == "menu_file_export") { m_req_export_image = true; return true; }
    if (id_or_label == "menu_file_export_as") { m_req_export_image_as = true; return true; }
    if (id_or_label == "menu_file_exit") {
        if (m_hwnd) PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        return true;
    }
    if (id_or_label == "menu_settings_prefs") {
        select_right_panel_tab(5);
        m_req_open_prefs = true;
        return true;
    }
    if (id_or_label == "modal_exit_btn_save") {
        m_req_save_and_exit = true;
        close_unsaved_exit_modal();
        return true;
    }
    if (id_or_label == "modal_exit_btn_dont_save") {
        m_req_force_exit_without_saving = true;
        close_unsaved_exit_modal();
        return true;
    }
    if (id_or_label == "modal_exit_btn_cancel") {
        close_unsaved_exit_modal();
        return true;
    }

    // Rotate Tool Options
    if (id_or_label == "rot_btn_ccw90") { apply_rotation_delta(m_last_angle - 90.0f); return true; }
    if (id_or_label == "rot_btn_cw90") { apply_rotation_delta(m_last_angle + 90.0f); return true; }
    if (id_or_label == "rot_btn_flip_h") {
        m_horizon_state.flip_horizontal = !m_horizon_state.flip_horizontal;
        m_req_flip_horizontal = true;
        if (on_flip_layer) on_flip_layer(true, false);
        mark_active_document_dirty();
        return true;
    }
    if (id_or_label == "rot_btn_flip_v") {
        m_horizon_state.flip_vertical = !m_horizon_state.flip_vertical;
        m_req_flip_vertical = true;
        if (on_flip_layer) on_flip_layer(false, true);
        mark_active_document_dirty();
        return true;
    }
    if (id_or_label == "rot_btn_reset") {
        m_horizon_state.flip_horizontal = false;
        m_horizon_state.flip_vertical = false;
        apply_rotation_delta(0.0f);
        return true;
    }
    if (id_or_label == "rot_chk_constrain") {
        m_horizon_state.auto_scale_to_fit = !m_horizon_state.auto_scale_to_fit;
        m_req_set_viewport_angle = true;
        m_target_viewport_angle = m_last_angle;
        if (on_rotate_apply) on_rotate_apply(m_last_angle, m_horizon_state.auto_scale_to_fit);
        return true;
    }

    // Move Tool Options
    if (id_or_label == "move_btn_center") {
        if (!m_active_layer_view.lock_position) {
            m_active_layer_view.offset_x = 0.0f;
            m_active_layer_view.offset_y = 0.0f;
            m_req_center_layer = true;
            if (on_center_layer) on_center_layer();
            mark_active_document_dirty();
        }
        return true;
    }
    if (id_or_label == "move_chk_bounds") { m_show_layer_bounds = !m_show_layer_bounds; return true; }

    // Crop Tool Options
    if (id_or_label == "crop_btn_swap") { m_crop_state.swap_orientation(m_last_img_w, m_last_img_h); return true; }
    if (id_or_label == "crop_btn_reset") { m_crop_state.reset(m_last_img_w, m_last_img_h); return true; }
    if (id_or_label == "crop_btn_cancel") { cancel_crop_tool(); return true; }
    if (id_or_label == "crop_btn_apply") { trigger_apply_crop(); return true; }
    if (id_or_label == "crop_preset_freeform") {
        m_crop_state.preset = CropToolState::Preset::Freeform;
        m_crop_state.aspect_swapped = false;
        return true;
    }
    if (id_or_label == "crop_preset_original") {
        m_crop_state.preset = CropToolState::Preset::Original;
        m_crop_state.aspect_swapped = false;
        m_crop_state.apply_preset_ratio(m_last_img_w, m_last_img_h);
        return true;
    }
    if (id_or_label == "crop_preset_1_1") {
        m_crop_state.preset = CropToolState::Preset::Square_1_1;
        m_crop_state.aspect_swapped = false;
        m_crop_state.apply_preset_ratio(m_last_img_w, m_last_img_h);
        return true;
    }
    if (id_or_label == "crop_preset_16_9") {
        m_crop_state.preset = CropToolState::Preset::Wide_16_9;
        m_crop_state.aspect_swapped = false;
        m_crop_state.apply_preset_ratio(m_last_img_w, m_last_img_h);
        return true;
    }
    if (id_or_label == "crop_preset_9_16") {
        m_crop_state.preset = CropToolState::Preset::Vertical_9_16;
        m_crop_state.aspect_swapped = false;
        m_crop_state.apply_preset_ratio(m_last_img_w, m_last_img_h);
        return true;
    }
    if (id_or_label == "crop_preset_4_3") {
        m_crop_state.preset = CropToolState::Preset::Photo_4_3;
        m_crop_state.aspect_swapped = false;
        m_crop_state.apply_preset_ratio(m_last_img_w, m_last_img_h);
        return true;
    }
    if (id_or_label == "crop_preset_3_2") {
        m_crop_state.preset = CropToolState::Preset::DSLR_3_2;
        m_crop_state.aspect_swapped = false;
        m_crop_state.apply_preset_ratio(m_last_img_w, m_last_img_h);
        return true;
    }
    if (id_or_label == "crop_preset_4_5") {
        m_crop_state.preset = CropToolState::Preset::Portrait_4_5;
        m_crop_state.aspect_swapped = false;
        m_crop_state.apply_preset_ratio(m_last_img_w, m_last_img_h);
        return true;
    }
    if (id_or_label == "crop_preset_21_9") {
        m_crop_state.preset = CropToolState::Preset::Cinema_21_9;
        m_crop_state.aspect_swapped = false;
        m_crop_state.apply_preset_ratio(m_last_img_w, m_last_img_h);
        return true;
    }

    // View & AI Inpaint controls
    if (id_or_label == "opt_fit" || id_or_label == "status_btn_fit") { m_req_fit_view = true; return true; }
    if (id_or_label == "opt_100" || id_or_label == "status_btn_100") { m_req_100_view = true; return true; }
    if (id_or_label == "opt_200" || id_or_label == "status_btn_200") { m_req_200_view = true; return true; }
    if (id_or_label == "opt_static_zoom" || id_or_label == "menu_opt_static_zoom") {
        m_static_zoom = !m_static_zoom;
        return true;
    }
    if (id_or_label == "opt_undo" || id_or_label == "erase_btn_undo") { undo_erase_stroke(); return true; }
    if (id_or_label == "opt_redo") { redo_erase_stroke(); return true; }
    if (id_or_label == "opt_clear" || id_or_label == "erase_btn_clear") {
        clear_erase_strokes();
        m_req_clear_mask = true;
        return true;
    }
    if (id_or_label == "erase_checkbox_subtract" || id_or_label == "opt_subtract") {
        m_erase_subtract = !m_erase_subtract;
        return true;
    }
    if (id_or_label == "erase_checkbox_auto_apply") {
        EraseTool::get().auto_apply = !EraseTool::get().auto_apply;
        return true;
    }
    if (id_or_label == "erase_btn_inpaint") { request_inpaint(); return true; }
    if (id_or_label == "erase_btn_undo_erase") { return EraseTool::get().undo_erase(); }
    if (id_or_label == "erase_btn_redo_erase") { return EraseTool::get().redo_erase(); }
    if (id_or_label == "erase_btn_regen_depth" || id_or_label == "depth_btn_generate") {
        if (!m_depth_is_busy) {
            m_req_generate_depth = true;
            m_depth_status_text = "Running Depth-Anything-V2 (" + m_depth_encoder + ")...";
            m_depth_is_busy = true;
        }
        return true;
    }
    if (id_or_label == "close_tab" || id_or_label == "tab_close") { request_close_active_tab(); return true; }
    if (id_or_label == "btn_move_up") {
        if (m_selected_effect_idx > 0 && m_selected_effect_idx < static_cast<int>(m_effects.size())) {
            std::swap(m_effects[m_selected_effect_idx], m_effects[m_selected_effect_idx - 1]);
            m_selected_effect_idx--;
            return true;
        }
    }
    if (id_or_label == "btn_move_down") {
        if (m_selected_effect_idx >= 0 && m_selected_effect_idx + 1 < static_cast<int>(m_effects.size())) {
            std::swap(m_effects[m_selected_effect_idx], m_effects[m_selected_effect_idx + 1]);
            m_selected_effect_idx++;
            return true;
        }
    }
    if (id_or_label.rfind("shader_select_", 0) == 0) {
        try {
            const int idx = std::stoi(id_or_label.substr(14));
            if (idx >= 0 && idx < static_cast<int>(m_effects.size())) {
                m_selected_effect_idx = idx;
                return true;
            }
        } catch (...) {}
    }
    if (id_or_label.rfind("shader_checkbox_", 0) == 0) {
        try {
            const size_t idx = static_cast<size_t>(std::stoul(id_or_label.substr(16)));
            if (idx < m_effects.size()) {
                m_effects[idx].enabled = !m_effects[idx].enabled;
                return true;
            }
        } catch (...) {}
    }
    if (id_or_label.rfind("tab_doc_", 0) == 0) {
        try {
            const uint32_t tid = static_cast<uint32_t>(std::stoul(id_or_label.substr(8)));
            for (size_t i = 0; i < m_tabs.size(); ++i) {
                if (m_tabs[i].id == tid) {
                    select_tab(static_cast<int>(i));
                    m_req_switch_tab = static_cast<int>(i);
                    return true;
                }
            }
        } catch (...) {}
    }

    const UiDomNode *n = m_dom.find(id_or_label);
    if (!n) return false;
    if (n->id == "tab_shaders") { select_right_panel_tab(0); return true; }
    if (n->id == "tab_depth" || n->id == "accordion_depth") { select_right_panel_tab(1); return true; }
    if (n->id == "tab_ai_inpaint" || n->id == "tab_erase" || n->id == "accordion_erase") {
        select_right_panel_tab(2);
        return true;
    }
    if (n->id == "tab_upscale") { select_right_panel_tab(3); return true; }
    if (n->id == "tab_text") { select_right_panel_tab(4); return true; }
    if (n->id == "tab_prefs") { select_right_panel_tab(5); return true; }

    const float cx = n->x + n->w * 0.5f;
    const float cy = n->y + n->h * 0.5f;
    queue_mouse_event(cx, cy, 1);
    return true;
}

} // namespace gui
