#include "gui_manager.h"
#include "theme.h"
#include "menu_bar.h"
#include "tool_strip.h"
#include "tab_bar.h"
#include "left_panel.h"
#include "right_panel.h"
#include "canvas.h"
#include "status_bar.h"
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <algorithm>
#include <cmath>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace gui {

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
    io.IniFilename = nullptr; // Stateless / clean startup

    apply_photoshop_gray_theme();

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

void GuiManager::push_erase_undo() {
    if (m_erase_undo_stack.size() >= 30) {
        m_erase_undo_stack.erase(m_erase_undo_stack.begin());
    }
    m_erase_undo_stack.push_back(m_erase_strokes);
    m_erase_redo_stack.clear();
}

bool GuiManager::undo_erase_stroke() {
    if (m_erase_undo_stack.empty()) {
        if (!m_erase_strokes.empty()) {
            m_erase_redo_stack.push_back(std::move(m_erase_strokes));
            m_erase_strokes.clear();
            return true;
        }
        return false;
    }
    m_erase_redo_stack.push_back(std::move(m_erase_strokes));
    m_erase_strokes = std::move(m_erase_undo_stack.back());
    m_erase_undo_stack.pop_back();
    return true;
}

bool GuiManager::redo_erase_stroke() {
    if (m_erase_redo_stack.empty()) return false;
    m_erase_undo_stack.push_back(std::move(m_erase_strokes));
    m_erase_strokes = std::move(m_erase_redo_stack.back());
    m_erase_redo_stack.pop_back();
    return true;
}

void GuiManager::clear_erase_strokes() {
    if (!m_erase_strokes.empty()) {
        push_erase_undo();
    }
    m_erase_strokes.clear();
}

void GuiManager::subtract_brush_dab(float uv_x, float uv_y, float radius_uv_x, float radius_uv_y) {
    if (m_erase_strokes.empty()) return;
    float rx2 = radius_uv_x * radius_uv_x;
    float ry2 = radius_uv_y * radius_uv_y;

    std::vector<std::vector<ImVec2>> new_strokes;
    for (const auto &stroke : m_erase_strokes) {
        std::vector<ImVec2> current_seg;
        for (const auto &pt : stroke) {
            float dx = pt.x - uv_x;
            float dy = pt.y - uv_y;
            // Elliptical distance metric corresponding to aspect ratio
            bool inside = ((dx * dx) / (rx2 > 0.0f ? rx2 : 0.0001f) +
                           (dy * dy) / (ry2 > 0.0f ? ry2 : 0.0001f)) <= 1.0f;
            if (!inside) {
                current_seg.push_back(pt);
            } else {
                if (!current_seg.empty()) {
                    new_strokes.push_back(std::move(current_seg));
                    current_seg.clear();
                }
            }
        }
        if (!current_seg.empty()) {
            new_strokes.push_back(std::move(current_seg));
        }
    }
    m_erase_strokes = std::move(new_strokes);
}

bool GuiManager::input_hook(void *user_data, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *self = static_cast<GuiManager *>(user_data);
    if (!self || !self->m_initialized) return false;

    POINT pt = {};
    GetCursorPos(&pt);
    ScreenToClient(self->m_hwnd, &pt);

    RECT client_rc = {};
    GetClientRect(self->m_hwnd, &client_rc);
    float screen_w = static_cast<float>(client_rc.right - client_rc.left);
    float screen_h = static_cast<float>(client_rc.bottom - client_rc.top);
    float menu_h = self->get_menu_bar_height();
    float tool_options_h = self->get_tool_options_height();
    float toolbar_w = self->get_toolbar_width();
    float status_bar_h = self->get_status_bar_height();

    float left_w = self->get_left_panel_width(static_cast<uint32_t>(screen_w));
    float right_w = self->get_right_panel_width(static_cast<uint32_t>(screen_w));
    float viewable_x = toolbar_w + left_w;
    float viewable_y = menu_h + tool_options_h;
    float viewable_w = screen_w - viewable_x - right_w;
    float viewable_h = screen_h - viewable_y - status_bar_h;

    float left_splitter_x = toolbar_w + left_w;
    float right_splitter_x = screen_w - right_w;

    bool near_left_splitter = (!self->m_hide_panels && pt.y >= viewable_y && pt.y <= viewable_y + viewable_h && std::abs(static_cast<float>(pt.x) - left_splitter_x) <= 5.0f);
    bool near_right_splitter = (!self->m_hide_panels && pt.y >= viewable_y && pt.y <= viewable_y + viewable_h && std::abs(static_cast<float>(pt.x) - right_splitter_x) <= 5.0f);

    float split_norm = std::clamp(0.5f + self->m_before_after_split, 0.05f, 0.95f);
    float split_x = viewable_x + viewable_w * split_norm;
    bool near_split_line = self->m_before_after && (pt.y >= viewable_y && pt.y <= viewable_y + viewable_h && std::abs(static_cast<float>(pt.x) - split_x) <= 8.0f);

    if (msg == WM_LBUTTONDBLCLK) {
        if (near_left_splitter) {
            self->m_left_panel_width = 280.0f;
            return true;
        }
        if (near_right_splitter) {
            self->m_right_panel_width = 340.0f;
            return true;
        }
    }

    if (msg == WM_SETCURSOR) {
        if (self->m_resizing_left || self->m_resizing_right || near_left_splitter || near_right_splitter) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return true;
        }
        if (self->m_is_dragging_split || near_split_line) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return true;
        }
    }

    if (msg == WM_LBUTTONDOWN) {
        if (near_left_splitter) {
            self->m_resizing_left = true;
            SetCapture(self->m_hwnd);
            return true;
        }
        if (near_right_splitter) {
            self->m_resizing_right = true;
            SetCapture(self->m_hwnd);
            return true;
        }
        if (near_split_line) {
            self->m_is_dragging_split = true;
            SetCapture(self->m_hwnd);
            return true;
        }
    }

    if (msg == WM_MOUSEMOVE) {
        if (self->m_resizing_left) {
            float new_w = static_cast<float>(pt.x) - toolbar_w;
            self->m_left_panel_width = std::clamp(new_w, 240.0f, screen_w * 0.45f);
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return true;
        }
        if (self->m_resizing_right) {
            float new_w = screen_w - static_cast<float>(pt.x);
            self->m_right_panel_width = std::clamp(new_w, 300.0f, screen_w * 0.45f);
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return true;
        }
        if (self->m_is_dragging_split) {
            float rel_x = (static_cast<float>(pt.x) - viewable_x) / (viewable_w > 0.0f ? viewable_w : 1.0f);
            self->m_before_after_split = std::clamp(rel_x - 0.5f, -0.45f, 0.45f);
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return true;
        }
    }

    if (msg == WM_LBUTTONUP) {
        if (self->m_resizing_left || self->m_resizing_right) {
            self->m_resizing_left = false;
            self->m_resizing_right = false;
            ReleaseCapture();
            return true;
        }
        if (self->m_is_dragging_split) {
            self->m_is_dragging_split = false;
            ReleaseCapture();
            return true;
        }
    }

    if (ImGui_ImplWin32_WndProcHandler(self->m_hwnd, msg, wParam, lParam)) {
        return true;
    }

    bool in_canvas_rect = (pt.x > viewable_x + 4.0f && pt.x < (viewable_x + viewable_w) - 4.0f &&
                           pt.y >= viewable_y && pt.y <= viewable_y + viewable_h);
    bool popup_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);

    // erase brush painting in canvas
    if (self->m_erase_active && self->m_active_tool == ActiveTool::Erase && in_canvas_rect && !popup_open) {
        bool space_held = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        if (!space_held) {
            float fit_scale = (self->m_last_img_w > 0 && self->m_last_img_h > 0) ?
                (std::min)(self->m_canvas_view_w / static_cast<float>(self->m_last_img_w),
                           self->m_canvas_view_h / static_cast<float>(self->m_last_img_h)) : 1.0f;
            float eff_zoom = self->m_last_zoom * fit_scale;
            if (eff_zoom <= 0.001f) eff_zoom = 1.0f;

            float cx = self->m_canvas_view_x + self->m_canvas_view_w * 0.5f;
            float cy = self->m_canvas_view_y + self->m_canvas_view_h * 0.5f;

            float img_px_x = (static_cast<float>(pt.x) - cx) / eff_zoom + self->m_last_pan_x + (self->m_last_img_w * 0.5f);
            float img_px_y = (static_cast<float>(pt.y) - cy) / eff_zoom + self->m_last_pan_y + (self->m_last_img_h * 0.5f);

            bool alt_held = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            bool is_sub = self->m_erase_subtract || alt_held;

            float margin_px = self->m_erase_radius + 4.0f;
            bool over_image = (self->m_last_img_w > 0 && self->m_last_img_h > 0 &&
                               img_px_x >= -margin_px && img_px_x <= self->m_last_img_w + margin_px &&
                               img_px_y >= -margin_px && img_px_y <= self->m_last_img_h + margin_px);

            float clamped_px_x = std::clamp(img_px_x, 0.0f, static_cast<float>(self->m_last_img_w));
            float clamped_px_y = std::clamp(img_px_y, 0.0f, static_cast<float>(self->m_last_img_h));

            float uv_x = (self->m_last_img_w > 0) ? (clamped_px_x / static_cast<float>(self->m_last_img_w)) : 0.0f;
            float uv_y = (self->m_last_img_h > 0) ? (clamped_px_y / static_cast<float>(self->m_last_img_h)) : 0.0f;

            float rad_uv_x = (self->m_last_img_w > 0) ? (self->m_erase_radius / static_cast<float>(self->m_last_img_w)) : 0.01f;
            float rad_uv_y = (self->m_last_img_h > 0) ? (self->m_erase_radius / static_cast<float>(self->m_last_img_h)) : 0.01f;

            if (msg == WM_LBUTTONDOWN) {
                if (over_image) {
                    self->m_is_painting_mask = true;
                    self->push_erase_undo();
                    if (is_sub) {
                        self->subtract_brush_dab(uv_x, uv_y, rad_uv_x, rad_uv_y);
                    } else {
                        self->m_current_stroke.clear();
                        self->m_current_stroke.push_back(ImVec2(uv_x, uv_y));
                    }
                    SetCapture(self->m_hwnd);
                    return true;
                }
            }
            if (msg == WM_MOUSEMOVE && self->m_is_painting_mask) {
                if (is_sub) {
                    self->subtract_brush_dab(uv_x, uv_y, rad_uv_x, rad_uv_y);
                } else {
                    if (self->m_current_stroke.empty() ||
                        std::hypot(self->m_current_stroke.back().x - uv_x, self->m_current_stroke.back().y - uv_y) > 0.001f) {
                        self->m_current_stroke.push_back(ImVec2(uv_x, uv_y));
                    }
                }
                return true;
            }
            if (msg == WM_LBUTTONUP && self->m_is_painting_mask) {
                self->m_is_painting_mask = false;
                if (!is_sub && !self->m_current_stroke.empty()) {
                    self->m_erase_strokes.push_back(self->m_current_stroke);
                    self->m_current_stroke.clear();
                }
                ReleaseCapture();
                return true;
            }
        }
    }

    // hotkeys
    const ImGuiIO &io = ImGui::GetIO();
    if (msg == WM_KEYDOWN) {
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (wParam == VK_TAB && !io.WantTextInput) {
            self->toggle_panels_hidden();
            return true;
        } else if (wParam == 'H' && !ctrl && !io.WantTextInput) {
            self->set_active_tool(ActiveTool::Hand);
            return true;
        } else if (wParam == 'E' && !ctrl && !io.WantTextInput) {
            if (self->m_active_tool == ActiveTool::Erase) {
                self->set_active_tool(ActiveTool::Hand);
            } else {
                self->set_active_tool(ActiveTool::Erase);
            }
            return true;
        } else if (wParam == 'D' && !ctrl && !io.WantTextInput) {
            if (self->m_active_tool == ActiveTool::Depth) {
                self->set_active_tool(ActiveTool::Hand);
            } else {
                self->set_active_tool(ActiveTool::Depth);
            }
            return true;
        } else if (wParam == 'B' && !ctrl && !io.WantTextInput) {
            if (self->m_active_tool == ActiveTool::Split) {
                self->set_active_tool(ActiveTool::Hand);
            } else {
                self->set_active_tool(ActiveTool::Split);
            }
            return true;
        } else if (wParam == VK_OEM_5 && !ctrl && !io.WantTextInput) { // '\'
            self->m_master_bypass = !self->m_master_bypass;
            return true;
        } else if (wParam == '0' && ctrl) {
            self->m_req_fit_view = true;
            return true;
        } else if (wParam == '1' && ctrl) {
            self->m_req_100_view = true;
            return true;
        } else if (wParam == VK_OEM_4) { // '['
            self->m_erase_radius = (std::max)(2.0f, self->m_erase_radius - 4.0f);
            return true;
        } else if (wParam == VK_OEM_6) { // ']'
            self->m_erase_radius = (std::min)(256.0f, self->m_erase_radius + 4.0f);
            return true;
        } else if (wParam == 'Z' && ctrl && !shift && self->m_active_tool == ActiveTool::Erase) {
            self->undo_erase_stroke();
            return true;
        } else if (((wParam == 'Y' && ctrl) || (wParam == 'Z' && ctrl && shift)) && self->m_active_tool == ActiveTool::Erase) {
            self->redo_erase_stroke();
            return true;
        } else if (wParam == VK_ESCAPE) {
            if (self->m_active_tool == ActiveTool::Erase && !self->m_erase_strokes.empty()) {
                self->clear_erase_strokes();
                return true;
            }
            if (self->m_active_tool != ActiveTool::Hand) {
                self->set_active_tool(ActiveTool::Hand);
                return true;
            }
        }
    }

    if (in_canvas_rect && !popup_open) {
        return false;
    }

    if (io.WantCaptureMouse) {
        if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
            return true;
        }
    }

    if (io.WantTextInput || io.WantCaptureKeyboard) {
        if ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_CHAR) {
            return true;
        }
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
    ImGui::NewFrame();

    if (m_sim_mouse_down_prev) {
        ImGuiIO &io = ImGui::GetIO();
        io.MouseDown[0] = false;
        m_sim_mouse_down_prev = false;
    }

    while (!m_sim_mouse_queue.empty()) {
        auto ev = m_sim_mouse_queue.front();
        m_sim_mouse_queue.erase(m_sim_mouse_queue.begin());
        ImGuiIO &io = ImGui::GetIO();
        io.MousePos = ImVec2(ev.x, ev.y);
        if (ev.action == 1) { // Click
            io.MouseDown[0] = true;
            m_sim_mouse_down_prev = true;
        } else if (ev.action == 0) { // Release
            io.MouseDown[0] = false;
        }
    }
}

void GuiManager::render(uint32_t screen_w, uint32_t screen_h,
                       uint32_t img_w, uint32_t img_h,
                       float zoom_level, float pan_x, float pan_y) {
    if (!m_initialized) return;

    float menu_height = get_menu_bar_height();
    float tool_options_h = get_tool_options_height();
    float toolbar_w = get_toolbar_width();
    float status_bar_h = get_status_bar_height();

    float left_w = get_left_panel_width(screen_w);
    float right_w = get_right_panel_width(screen_w);
    float total_w = static_cast<float>(screen_w);
    float total_h = static_cast<float>(screen_h);

    float full_body_y = menu_height;
    float full_body_h = total_h - full_body_y - status_bar_h;
    if (full_body_h < 64.0f) full_body_h = 64.0f;

    float viewable_x = toolbar_w + left_w;
    float viewable_w = total_w - viewable_x - right_w;
    if (viewable_w < 64.0f) viewable_w = 64.0f;

    float canvas_y = menu_height + tool_options_h;
    float canvas_h = total_h - canvas_y - status_bar_h;
    if (canvas_h < 64.0f) canvas_h = 64.0f;

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

    render_center_canvas(*this, viewable_x, canvas_y, viewable_w, canvas_h, img_w, img_h, zoom_level, pan_x, pan_y);

    if (!m_hide_panels && right_w > 0.0f) {
        render_right_panel(*this, total_w - right_w, full_body_y, right_w, full_body_h);
    }

    render_bottom_status_bar(*this, total_h - status_bar_h, total_w, status_bar_h, img_w, img_h, zoom_level, pan_x, pan_y);

    render_menu_bar(*this);

    if (m_show_about_dialog) {
        ImGui::OpenPopup("About ShaderLab");
        m_show_about_dialog = false;
    }

    if (ImGui::BeginPopupModal("About ShaderLab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(260.0f);
        ImGui::TextUnformatted("ShaderLab");
        ImGui::Separator();
        ImGui::TextUnformatted("Version 1.3.0");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(-1.0f, 26.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void GuiManager::end_frame() {
    if (!m_initialized) return;
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void GuiManager::simulate_canvas_mouse(float x, float y, int act) {
    if (m_active_tool == ActiveTool::Erase && m_erase_active && m_last_img_w > 0 && m_last_img_h > 0) {
        float fit_scale = (m_last_img_w > 0 && m_last_img_h > 0) ?
            (std::min)(m_canvas_view_w / static_cast<float>(m_last_img_w),
                       m_canvas_view_h / static_cast<float>(m_last_img_h)) : 1.0f;
        float eff_zoom = m_last_zoom * fit_scale;
        if (eff_zoom <= 0.001f) eff_zoom = 1.0f;

        float cx = m_canvas_view_x + m_canvas_view_w * 0.5f;
        float cy = m_canvas_view_y + m_canvas_view_h * 0.5f;

        float img_px_x = (x - cx) / eff_zoom + m_last_pan_x + (m_last_img_w * 0.5f);
        float img_px_y = (y - cy) / eff_zoom + m_last_pan_y + (m_last_img_h * 0.5f);

        float clamped_px_x = std::clamp(img_px_x, 0.0f, static_cast<float>(m_last_img_w));
        float clamped_px_y = std::clamp(img_px_y, 0.0f, static_cast<float>(m_last_img_h));

        float uv_x = clamped_px_x / static_cast<float>(m_last_img_w);
        float uv_y = clamped_px_y / static_cast<float>(m_last_img_h);

        bool is_sub = m_erase_subtract;
        float rad_uv_x = m_erase_radius / static_cast<float>(m_last_img_w);
        float rad_uv_y = m_erase_radius / static_cast<float>(m_last_img_h);

        if (act == 1) { // down
            m_is_painting_mask = true;
            push_erase_undo();
            if (is_sub) {
                subtract_brush_dab(uv_x, uv_y, rad_uv_x, rad_uv_y);
            } else {
                m_current_stroke.clear();
                m_current_stroke.push_back(ImVec2(uv_x, uv_y));
            }
        } else if ((act == 2 || act == 0) && m_is_painting_mask) { // move
            if (is_sub) {
                subtract_brush_dab(uv_x, uv_y, rad_uv_x, rad_uv_y);
            } else {
                m_current_stroke.push_back(ImVec2(uv_x, uv_y));
            }
        } else if (act == 3 && m_is_painting_mask) { // up
            m_is_painting_mask = false;
            if (!is_sub && !m_current_stroke.empty()) {
                m_erase_strokes.push_back(m_current_stroke);
                m_current_stroke.clear();
            }
        }
    }
}

bool GuiManager::click_dom_element(const std::string &id_or_label) {
    if (id_or_label == "tab_shaders") { select_right_panel_tab(0); return true; }
    if (id_or_label == "tab_depth" || id_or_label == "accordion_depth") { select_right_panel_tab(1); return true; }
    if (id_or_label == "tab_erase" || id_or_label == "accordion_erase") { select_right_panel_tab(2); return true; }
    if (id_or_label == "tool_hand" || id_or_label == "tool_H" || id_or_label == "tool_h") { set_active_tool(ActiveTool::Hand); return true; }
    if (id_or_label == "tool_erase" || id_or_label == "tool_E" || id_or_label == "tool_e") { set_active_tool(ActiveTool::Erase); return true; }
    if (id_or_label == "tool_depth" || id_or_label == "tool_D" || id_or_label == "tool_d") { set_active_tool(ActiveTool::Depth); return true; }
    if (id_or_label == "tool_split" || id_or_label == "tool_B" || id_or_label == "tool_b") { set_active_tool(ActiveTool::Split); return true; }
    if (id_or_label == "tool_bypass" || id_or_label == "tool_\\") { m_master_bypass = !m_master_bypass; return true; }
    if (id_or_label == "opt_fit" || id_or_label == "status_btn_fit") { m_req_fit_view = true; return true; }
    if (id_or_label == "opt_100" || id_or_label == "status_btn_100") { m_req_100_view = true; return true; }
    if (id_or_label == "opt_200" || id_or_label == "status_btn_200") { m_req_200_view = true; return true; }
    if (id_or_label == "opt_static_zoom" || id_or_label == "menu_opt_static_zoom") { m_static_zoom = !m_static_zoom; return true; }
    if (id_or_label == "opt_undo" || id_or_label == "erase_btn_undo") { undo_erase_stroke(); return true; }
    if (id_or_label == "opt_redo") { redo_erase_stroke(); return true; }
    if (id_or_label == "opt_clear" || id_or_label == "erase_btn_clear") { clear_erase_strokes(); m_req_clear_mask = true; return true; }
    if (id_or_label == "erase_checkbox_subtract" || id_or_label == "opt_subtract") { m_erase_subtract = !m_erase_subtract; return true; }
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
            int idx = std::stoi(id_or_label.substr(14));
            if (idx >= 0 && idx < static_cast<int>(m_effects.size())) {
                m_selected_effect_idx = idx;
                return true;
            }
        } catch (...) {}
    }
    if (id_or_label.rfind("shader_checkbox_", 0) == 0) {
        try {
            size_t idx = static_cast<size_t>(std::stoul(id_or_label.substr(16)));
            if (idx < m_effects.size()) {
                m_effects[idx].enabled = !m_effects[idx].enabled;
                return true;
            }
        } catch (...) {}
    }
    if (id_or_label.rfind("tab_doc_", 0) == 0) {
        try {
            uint32_t tid = static_cast<uint32_t>(std::stoul(id_or_label.substr(8)));
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
    if (n->id == "tab_erase" || n->id == "accordion_erase") { select_right_panel_tab(2); return true; }

    float cx = n->x + n->w * 0.5f;
    float cy = n->y + n->h * 0.5f;
    queue_mouse_event(cx, cy, 1);
    return true;
}

} // namespace gui
