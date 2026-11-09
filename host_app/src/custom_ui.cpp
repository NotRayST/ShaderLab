#include "custom_ui.h"
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>

static bool open_file_dialog(HWND hwnd, const wchar_t *filter, std::wstring &out_path) {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        out_path = filename;
        return true;
    }
    return false;
}

static bool save_file_dialog(HWND hwnd, const wchar_t *filter, const wchar_t *def_ext, std::wstring &out_path) {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = def_ext;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) {
        out_path = filename;
        return true;
    }
    return false;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

CustomUI::CustomUI() {
    add_sample_effects();
}

CustomUI::~CustomUI() {
    shutdown();
}

bool CustomUI::initialize(HWND hwnd, ID3D11Device *device, ID3D11DeviceContext *context) {
    if (m_initialized) return true;
    m_hwnd = hwnd;
    m_device = device;
    m_context = context;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

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

    m_initialized = true;
    return true;
}

void CustomUI::shutdown() {
    if (!m_initialized) return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
    m_hwnd = nullptr;
    m_device = nullptr;
    m_context = nullptr;
}
void CustomUI::push_erase_undo() {
    if (m_erase_undo_stack.size() >= 30) {
        m_erase_undo_stack.erase(m_erase_undo_stack.begin());
    }
    m_erase_undo_stack.push_back(m_erase_strokes);
    m_erase_redo_stack.clear();
}

bool CustomUI::undo_erase_stroke() {
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

bool CustomUI::redo_erase_stroke() {
    if (m_erase_redo_stack.empty()) return false;
    m_erase_undo_stack.push_back(std::move(m_erase_strokes));
    m_erase_strokes = std::move(m_erase_redo_stack.back());
    m_erase_redo_stack.pop_back();
    return true;
}

void CustomUI::clear_erase_strokes() {
    if (!m_erase_strokes.empty()) {
        push_erase_undo();
    }
    m_erase_strokes.clear();
    m_current_stroke.clear();
}

void CustomUI::subtract_brush_dab(float uv_x, float uv_y, float radius_uv_x, float radius_uv_y) {
    if (radius_uv_x <= 0.0f || radius_uv_y <= 0.0f) return;
    float rx2 = radius_uv_x * radius_uv_x;
    float ry2 = radius_uv_y * radius_uv_y;

    std::vector<std::vector<ImVec2>> new_strokes;
    for (const auto &stroke : m_erase_strokes) {
        std::vector<ImVec2> current_sub_stroke;
        for (const auto &pt : stroke) {
            float dx = pt.x - uv_x;
            float dy = pt.y - uv_y;
            float dist_sq = (dx * dx) / rx2 + (dy * dy) / ry2;
            if (dist_sq > 1.0f) {
                current_sub_stroke.push_back(pt);
            } else {
                if (!current_sub_stroke.empty()) {
                    new_strokes.push_back(current_sub_stroke);
                    current_sub_stroke.clear();
                }
            }
        }
        if (!current_sub_stroke.empty()) {
            new_strokes.push_back(current_sub_stroke);
        }
    }
    m_erase_strokes = std::move(new_strokes);
}


bool CustomUI::input_hook(void *user_data, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *self = static_cast<CustomUI *>(user_data);
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

bool CustomUI::want_capture_mouse() const {
    if (!m_initialized) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

bool CustomUI::want_capture_keyboard() const {
    if (!m_initialized) return false;
    return ImGui::GetIO().WantCaptureKeyboard;
}

float CustomUI::get_left_panel_width(uint32_t screen_w) const {
    if (m_hide_panels) return 0.0f;
    float max_w = static_cast<float>(screen_w) * 0.45f;
    return std::clamp(m_left_panel_width, 240.0f, (std::max)(240.0f, max_w));
}

float CustomUI::get_right_panel_width(uint32_t screen_w) const {
    if (m_hide_panels) return 0.0f;
    float max_w = static_cast<float>(screen_w) * 0.45f;
    return std::clamp(m_right_panel_width, 300.0f, (std::max)(300.0f, max_w));
}

float CustomUI::get_menu_bar_height() const {
    if (!m_initialized) return 24.0f;
    return ImGui::GetFrameHeight();
}

void CustomUI::apply_photoshop_gray_theme() {
    ImGuiStyle &style = ImGui::GetStyle();

    style.WindowRounding = 0.0f;
    style.ChildRounding = 2.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 2.0f;
    style.ScrollbarRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 2.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;

    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize = 12.0f;

    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.88f, 0.88f, 0.90f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.50f, 0.52f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.14f, 0.14f, 0.15f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_Border]                = ImVec4(0.26f, 0.26f, 0.28f, 1.00f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.24f, 0.24f, 0.27f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.14f, 0.14f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.16f, 0.16f, 0.17f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.12f, 0.12f, 0.13f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.14f, 0.14f, 0.15f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.14f, 0.14f, 0.15f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.30f, 0.30f, 0.32f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.38f, 0.38f, 0.40f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.44f, 0.44f, 0.48f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.35f, 0.65f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.35f, 0.55f, 0.85f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.45f, 0.68f, 0.98f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.24f, 0.24f, 0.26f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.36f, 0.36f, 0.40f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.18f, 0.40f, 0.70f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.48f, 0.80f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.28f, 0.54f, 0.88f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.26f, 0.26f, 0.28f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.36f, 0.36f, 0.40f, 1.00f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.46f, 0.46f, 0.52f, 1.00f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.28f, 0.28f, 0.30f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
}

void CustomUI::add_sample_effects() {
    ShaderLabEffect fx1;
    fx1.name = "Adaptive Sharpen";
    fx1.file_path = "shaders/AdaptiveSharpen.fx";
    fx1.engine_type = "ReShade FX";
    fx1.enabled = true;
    {
        ShaderLabUniform u;
        u.name = "curve_height";
        u.label = "Sharpening Strength";
        u.tooltip = "Controls main edge sharpening curve height";
        u.category = "Tuning";
        u.type = ShaderLabUniform::Type::Float;
        u.min_val = 0.0f;
        u.max_val = 2.5f;
        u.step = 0.05f;
        u.float_val = 1.0f;
        u.default_float = 1.0f;
        fx1.uniforms.push_back(u);
    }
    {
        ShaderLabUniform u;
        u.name = "video_level_out";
        u.label = "Video Levels Output";
        u.tooltip = "Compress output to standard video ranges";
        u.category = "Output";
        u.type = ShaderLabUniform::Type::Bool;
        u.bool_val = false;
        u.default_bool = false;
        fx1.uniforms.push_back(u);
    }
    m_effects.push_back(fx1);

    ShaderLabEffect fx2;
    fx2.name = "Vignette & Chromatic";
    fx2.file_path = "shaders/UnityVignette.shader";
    fx2.engine_type = "Unity Shader";
    fx2.enabled = false;
    {
        ShaderLabUniform u;
        u.name = "_VignetteIntensity";
        u.label = "Vignette Falloff";
        u.tooltip = "Outer border darkening intensity";
        u.category = "Lens Optics";
        u.type = ShaderLabUniform::Type::Float;
        u.min_val = 0.0f;
        u.max_val = 1.0f;
        u.step = 0.02f;
        u.float_val = 0.5f;
        u.default_float = 0.5f;
        fx2.uniforms.push_back(u);
    }
    {
        ShaderLabUniform u;
        u.name = "_VignetteColor";
        u.label = "Tint Color";
        u.tooltip = "Vignette gradient multiplier color";
        u.category = "Lens Optics";
        u.type = ShaderLabUniform::Type::Color4;
        u.color_val[0] = 0.05f;
        u.color_val[1] = 0.05f;
        u.color_val[2] = 0.08f;
        u.color_val[3] = 1.0f;
        u.default_color[0] = 0.05f;
        u.default_color[1] = 0.05f;
        u.default_color[2] = 0.08f;
        u.default_color[3] = 1.0f;
        fx2.uniforms.push_back(u);
    }
    m_effects.push_back(fx2);

    ShaderLabEffect fx3;
    fx3.name = "Film Grain & Color Grade";
    fx3.file_path = "shaders/GodotFilmGrain.gdshader";
    fx3.engine_type = "Godot GDShader";
    fx3.enabled = false;
    {
        ShaderLabUniform u;
        u.name = "grain_amount";
        u.label = "Film Grain Size";
        u.tooltip = "Simulated noise amount";
        u.category = "Film Stock";
        u.type = ShaderLabUniform::Type::Float;
        u.min_val = 0.0f;
        u.max_val = 0.5f;
        u.step = 0.01f;
        u.float_val = 0.12f;
        u.default_float = 0.12f;
        fx3.uniforms.push_back(u);
    }
    {
        ShaderLabUniform u;
        u.name = "color_temperature";
        u.label = "Color Profile";
        u.tooltip = "Color look";
        u.category = "Color";
        u.type = ShaderLabUniform::Type::Enum;
        u.enum_items = { "Neutral", "Warm", "Cool", "Monochrome" };
        u.enum_selected = 0;
        u.default_enum = 0;
        fx3.uniforms.push_back(u);
    }
    m_effects.push_back(fx3);
}

void CustomUI::begin_frame() {
    if (!m_initialized) return;
    m_dom.clear();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    static bool s_release_mouse = false;
    if (s_release_mouse) {
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        s_release_mouse = false;
    }

    if (!m_sim_mouse_queue.empty()) {
        auto ev = m_sim_mouse_queue.front();
        m_sim_mouse_queue.erase(m_sim_mouse_queue.begin());
        ImGuiIO &io = ImGui::GetIO();
        io.AddMousePosEvent(ev.x, ev.y);
        if (ev.action == 1) { // click: hold down this frame, release next frame
            io.AddMouseButtonEvent(0, true);
            s_release_mouse = true;
        } else if (ev.action == 2) { // down
            io.AddMouseButtonEvent(0, true);
        } else if (ev.action == 3) { // up
            io.AddMouseButtonEvent(0, false);
        }
    }

    ImGui::NewFrame();
}

void CustomUI::render_menu_bar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Image...", "Ctrl+O")) {
                std::wstring path;
                if (open_file_dialog(m_hwnd, L"Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files (*.*)\0*.*\0", path)) {
                    m_req_open_image = true;
                    m_open_image_path = path;
                }
            }
            if (ImGui::MenuItem("Save Snapshot...", "Ctrl+S")) {
                std::wstring path;
                if (save_file_dialog(m_hwnd, L"PNG Image (*.png)\0*.png\0JPEG Image (*.jpg)\0*.jpg\0All Files (*.*)\0*.*\0", L"png", path)) {
                    m_req_save_snapshot = true;
                    m_save_snapshot_path = path;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Attach .sldepth File...")) {
                std::wstring path;
                if (open_file_dialog(m_hwnd, L"ShaderLab Depth (*.sldepth)\0*.sldepth\0All Files (*.*)\0*.*\0", path)) {
                    m_req_attach_depth = true;
                    m_attach_depth_path = path;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Reset View", "Home")) {
                m_req_reset_view = true;
            }
            if (ImGui::MenuItem("Clear Erase Mask", "Esc")) {
                m_req_clear_mask = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Static Zoom (Center-anchored)", nullptr, &m_static_zoom)) {}
            register_dom_last_item("menu_opt_static_zoom", "Static Zoom (Center-anchored)", "menu_item", m_static_zoom);
            register_dom_last_item("opt_static_zoom", "Static Zoom", "menu_item", m_static_zoom);

            ImGui::Separator();
            if (ImGui::MenuItem("Invert Depth Buffer", nullptr, &m_depth_invert)) {}
            ImGui::Separator();
            if (ImGui::MenuItem("Hide Side Panels", "Tab", &m_hide_panels)) {}
            ImGui::EndMenu();
        } else {
            m_dom.register_item("opt_static_zoom", "Static Zoom", "setting", ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f), m_static_zoom, true);
        }

        if (ImGui::BeginMenu("Shaders")) {
            if (ImGui::MenuItem("Add ReShade Shader (.fx)...")) {}
            if (ImGui::MenuItem("Import Unity Shader (.shader)...")) {}
            if (ImGui::MenuItem("Import Godot Shader (.gdshader)...")) {}
            if (ImGui::MenuItem("Import Raw HLSL (.hlsl)...")) {}
            ImGui::Separator();
            if (ImGui::MenuItem("Reload All Shaders", "F5")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Fit Canvas to Screen", "Ctrl+0")) {
                m_req_fit_view = true;
            }
            if (ImGui::MenuItem("Actual Size (100%)", "Ctrl+1")) {
                m_req_100_view = true;
            }
            ImGui::Separator();
            bool split_active = (m_active_tool == ActiveTool::Split);
            if (ImGui::MenuItem("Before / After Split (B)", nullptr, &split_active)) {
                set_active_tool(split_active ? ActiveTool::Split : ActiveTool::Hand);
            }
            bool depth_active = (m_active_tool == ActiveTool::Depth);
            if (ImGui::MenuItem("Peek 3D Depth (D)", nullptr, &depth_active)) {
                set_active_tool(depth_active ? ActiveTool::Depth : ActiveTool::Hand);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About ShaderLab")) {
                m_show_about_dialog = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

namespace {
enum class ToolIconType {
    Hand,
    Erase,
    Depth,
    Split,
    Bypass
};
}

void CustomUI::render_tool_strip(float top_offset, float height) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, top_offset));
    ImGui::SetNextWindowSize(ImVec2(get_toolbar_width(), height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                            ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 6.0f));
    ImGui::Begin("##PhotoshopToolStrip", nullptr, flags);

    auto render_tool_btn = [&](const char *label, const char *tooltip, bool is_active, ToolIconType icon_type, auto onClick) {
        if (is_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.36f, 0.60f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.44f, 0.72f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.14f, 0.16f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.24f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.78f, 1.0f));
        }

        std::string btn_id = std::string("##btn_tool_") + label;
        if (ImGui::Button(btn_id.c_str(), ImVec2(28.0f, 28.0f))) {
            onClick();
        }
        std::string dom_id = std::string("tool_") + label;
        register_dom_last_item(dom_id, tooltip, "tool_button", is_active);
        if (strcmp(label, "H") == 0) register_dom_last_item("tool_hand", tooltip, "tool_button", is_active);
        else if (strcmp(label, "E") == 0) register_dom_last_item("tool_erase", tooltip, "tool_button", is_active);
        else if (strcmp(label, "D") == 0) register_dom_last_item("tool_depth", tooltip, "tool_button", is_active);
        else if (strcmp(label, "B") == 0) register_dom_last_item("tool_split", tooltip, "tool_button", is_active);
        bool is_hovered = ImGui::IsItemHovered();
        if (is_hovered) {
            ImGui::SetTooltip("%s", tooltip);
        }

        // Render vector icon centered in button rect
        ImVec2 p0 = ImGui::GetItemRectMin();
        ImVec2 p1 = ImGui::GetItemRectMax();
        ImDrawList *draw = ImGui::GetWindowDrawList();
        ImU32 icon_col = is_active ? IM_COL32(255, 255, 255, 255) :
                         (is_hovered ? IM_COL32(235, 235, 240, 255) : IM_COL32(175, 175, 180, 220));
        float cx = std::floor((p0.x + p1.x) * 0.5f);
        float cy = std::floor((p0.y + p1.y) * 0.5f);

        switch (icon_type) {
        case ToolIconType::Hand: {
            // Clean vector open-hand silhouette outline + finger separators
            ImVec2 poly[11] = {
                ImVec2(cx - 3.0f, cy + 6.5f),  // wrist left
                ImVec2(cx - 4.5f, cy + 3.0f),  // thumb base
                ImVec2(cx - 7.5f, cy + 1.2f),  // thumb knuckle
                ImVec2(cx - 6.8f, cy - 1.5f),  // thumb tip
                ImVec2(cx - 3.8f, cy + 0.2f),  // thumb cleft
                ImVec2(cx - 3.8f, cy - 5.5f),  // index tip
                ImVec2(cx - 0.8f, cy - 7.5f),  // middle tip
                ImVec2(cx + 1.8f, cy - 6.5f),  // ring tip
                ImVec2(cx + 4.4f, cy - 4.2f),  // pinky tip
                ImVec2(cx + 4.8f, cy + 2.5f),  // outer palm
                ImVec2(cx + 3.0f, cy + 6.5f)   // wrist right
            };
            draw->AddPolyline(poly, 11, icon_col, ImDrawFlags_Closed, 1.5f);
            draw->AddLine(ImVec2(cx - 1.8f, cy - 5.2f), ImVec2(cx - 1.8f, cy - 0.5f), icon_col, 1.0f);
            draw->AddLine(ImVec2(cx + 0.5f, cy - 6.5f), ImVec2(cx + 0.5f, cy - 0.5f), icon_col, 1.0f);
            draw->AddLine(ImVec2(cx + 2.6f, cy - 5.5f), ImVec2(cx + 2.6f, cy - 0.5f), icon_col, 1.0f);
            break;
        }
        case ToolIconType::Erase: {
            // Angled rectangular eraser block
            ImVec2 poly[4] = {
                ImVec2(cx - 5.5f, cy + 2.0f),
                ImVec2(cx - 2.0f, cy - 5.5f),
                ImVec2(cx + 5.5f, cy - 2.0f),
                ImVec2(cx + 2.0f, cy + 5.5f)
            };
            draw->AddPolyline(poly, 4, icon_col, ImDrawFlags_Closed, 1.5f);
            // Rubber divider band
            draw->AddLine(ImVec2(cx - 3.5f, cy), ImVec2(cx, cy + 3.5f), icon_col, 1.5f);
            // Surface erase stroke
            draw->AddLine(ImVec2(cx - 7.0f, cy + 6.5f), ImVec2(cx - 1.0f, cy + 6.5f), icon_col, 1.2f);
            break;
        }
        case ToolIconType::Depth: {
            // 3D Isometric Cube
            float r = 6.0f;
            float hr = r * 0.866f;
            float h = r * 0.5f;

            ImVec2 top(cx, cy - r);
            ImVec2 right(cx + hr, cy - h);
            ImVec2 center(cx, cy);
            ImVec2 left(cx - hr, cy - h);

            ImVec2 bottom_left(cx - hr, cy - h + r);
            ImVec2 bottom(cx, cy + r);
            ImVec2 bottom_right(cx + hr, cy - h + r);

            ImVec2 top_face[4] = { top, right, center, left };
            draw->AddPolyline(top_face, 4, icon_col, ImDrawFlags_Closed, 1.5f);

            draw->AddLine(left, bottom_left, icon_col, 1.5f);
            draw->AddLine(center, bottom, icon_col, 1.5f);
            draw->AddLine(right, bottom_right, icon_col, 1.5f);

            draw->AddLine(bottom_left, bottom, icon_col, 1.5f);
            draw->AddLine(bottom, bottom_right, icon_col, 1.5f);
            break;
        }
        case ToolIconType::Split: {
            // Split Comparison [ | ]
            ImVec2 r_min(cx - 7.0f, cy - 6.0f);
            ImVec2 r_max(cx + 7.0f, cy + 6.0f);
            draw->AddRect(r_min, r_max, icon_col, 1.5f, 0, 1.5f);

            draw->AddRectFilled(ImVec2(r_min.x + 1.0f, r_min.y + 1.0f),
                                ImVec2(cx, r_max.y - 1.0f),
                                is_active ? IM_COL32(255, 255, 255, 120) : IM_COL32(175, 175, 180, 70));

            draw->AddLine(ImVec2(cx, cy - 7.5f), ImVec2(cx, cy + 7.5f), icon_col, 1.5f);
            draw->AddLine(ImVec2(cx - 3.0f, cy), ImVec2(cx - 1.0f, cy), icon_col, 1.2f);
            draw->AddLine(ImVec2(cx + 1.0f, cy), ImVec2(cx + 3.0f, cy), icon_col, 1.2f);
            break;
        }
        case ToolIconType::Bypass: {
            // Power / Master Bypass Icon
            ImU32 pwr_col = is_active ? IM_COL32(255, 170, 50, 255) : icon_col;
            ImVec2 arc_center(cx, cy + 1.0f);
            draw->PathArcTo(arc_center, 5.5f, -2.1f, 5.24f, 20);
            draw->PathStroke(pwr_col, 0, 1.5f);
            draw->AddLine(ImVec2(cx, cy - 6.0f), ImVec2(cx, cy), pwr_col, 1.5f);
            break;
        }
        }

        ImGui::PopStyleColor(3);
        ImGui::Spacing();
    };

    render_tool_btn("H", "Hand Tool (H)\nPan and zoom the canvas", m_active_tool == ActiveTool::Hand, ToolIconType::Hand, [&]() {
        set_active_tool(ActiveTool::Hand);
    });

    render_tool_btn("E", "Erase Brush (E)\nPaint mask to remove objects with AI inpainting", m_active_tool == ActiveTool::Erase, ToolIconType::Erase, [&]() {
        if (m_active_tool == ActiveTool::Erase) {
            set_active_tool(ActiveTool::Hand);
        } else {
            set_active_tool(ActiveTool::Erase);
        }
    });

    render_tool_btn("D", "3D Depth (D)\nPeek or configure depth buffer", m_active_tool == ActiveTool::Depth, ToolIconType::Depth, [&]() {
        if (m_active_tool == ActiveTool::Depth) {
            set_active_tool(ActiveTool::Hand);
        } else {
            set_active_tool(ActiveTool::Depth);
        }
    });

    render_tool_btn("B", "Split Compare (B)\nBefore / After interactive split", m_active_tool == ActiveTool::Split, ToolIconType::Split, [&]() {
        if (m_active_tool == ActiveTool::Split) {
            set_active_tool(ActiveTool::Hand);
        } else {
            set_active_tool(ActiveTool::Split);
        }
    });

    ImGui::Separator();
    ImGui::Spacing();

    render_tool_btn("\\", "Master Bypass (\\)\nToggle all shaders to view raw original", m_master_bypass, ToolIconType::Bypass, [&]() {
        m_master_bypass = !m_master_bypass;
    });

    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(get_toolbar_width() - 0.5f, top_offset),
                  ImVec2(get_toolbar_width() - 0.5f, top_offset + height),
                  IM_COL32(36, 36, 40, 220), 1.0f);

    ImGui::End();
    ImGui::PopStyleVar();
}

void CustomUI::render_tool_options_bar(float top_offset, float width) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, top_offset));
    ImGui::SetNextWindowSize(ImVec2(width, get_tool_options_height()));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                            ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 4.0f));
    ImGui::Begin("##PhotoshopToolOptionsBar", nullptr, flags);

    if (m_active_tool == ActiveTool::Hand) {
        ImGui::SetCursorPosY((get_tool_options_height() - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "[ Hand / Pan ]");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", "|  Drag to pan (Space)  |  Zoom (Wheel)  |  Fit (Ctrl+0)  |  100% (Ctrl+1)");
    } else if (m_active_tool == ActiveTool::Erase) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "[ Erase Brush ]");
        ImGui::SameLine();
        if (ImGui::SmallButton("Exit (Esc)")) {
            set_active_tool(ActiveTool::Hand);
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Size:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("##opt_radius", &m_erase_radius, 4.0f, 128.0f, "%.0f px");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brush radius (Hotkey: [ or ])");
        ImGui::SameLine();
        ImGui::Checkbox("Subtract (Alt)", &m_erase_subtract);
        register_dom_last_item("opt_subtract", "Subtract (Alt)", "checkbox", m_erase_subtract);
        register_dom_last_item("erase_checkbox_subtract", "Subtract (Alt)", "checkbox", m_erase_subtract);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::SmallButton("Undo (Ctrl+Z)")) {
            undo_erase_stroke();
        }
        register_dom_last_item("opt_undo", "Undo (Ctrl+Z)", "button");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            clear_erase_strokes();
            m_req_clear_mask = true;
        }
        register_dom_last_item("opt_clear", "Clear", "button");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (m_inpaint_is_busy) {
            ImGui::BeginDisabled();
            ImGui::SmallButton(m_inpaint_status_text.empty() ? "Inpainting..." : m_inpaint_status_text.c_str());
            ImGui::EndDisabled();
        } else {
            bool has_mask = !m_erase_strokes.empty();
            if (!has_mask) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Inpaint Selection (LaMa)")) {
                m_req_inpaint = true;
                m_inpaint_status_text = "Running LaMa inpainting...";
                m_inpaint_is_busy = true;
            }
            if (!has_mask) ImGui::EndDisabled();
        }
    } else if (m_active_tool == ActiveTool::Depth) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "[ 3D Depth ]");
        ImGui::SameLine();
        if (ImGui::SmallButton("Exit (Esc)")) {
            set_active_tool(ActiveTool::Hand);
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Checkbox("Invert Depth", &m_depth_invert);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Far:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("##opt_far", &m_depth_far_plane, 10.0f, 5000.0f, "%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        const char *models[] = { "ViT-L", "ViT-B", "ViT-S" };
        static int cur_m = 0;
        if (ImGui::Combo("##opt_model", &cur_m, models, 3)) {
            if (cur_m == 0) m_depth_encoder = "vitl";
            else if (cur_m == 1) m_depth_encoder = "vitb";
            else m_depth_encoder = "vits";
        }
        ImGui::SameLine();
        if (m_depth_is_busy) {
            ImGui::BeginDisabled();
            ImGui::SmallButton("Generating Depth...");
            ImGui::EndDisabled();
        } else {
            if (ImGui::SmallButton("Generate Depth (AI)")) {
                m_req_generate_depth = true;
                m_depth_status_text = "Starting Depth Anything v2 (" + m_depth_encoder + ")...";
                m_depth_is_busy = true;
            }
        }
    } else if (m_active_tool == ActiveTool::Split) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "[ Split Comparison ]");
        ImGui::SameLine();
        if (ImGui::SmallButton("Exit (Esc)")) {
            set_active_tool(ActiveTool::Hand);
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Split:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("##opt_split", &m_before_after_split, -0.45f, 0.45f, "%.2f");
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            m_before_after_split = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Center")) {
            m_before_after_split = 0.0f;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("| Left: Before | Right: After");
    }

    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(0.0f, top_offset + get_tool_options_height() - 0.5f),
                  ImVec2(width, top_offset + get_tool_options_height() - 0.5f),
                  IM_COL32(36, 36, 40, 220), 1.0f);

    ImGui::End();
    ImGui::PopStyleVar();
}

void CustomUI::render_left_panel(float left_offset, float top_offset, float panel_width, float panel_height) {
    ImGui::SetNextWindowPos(ImVec2(left_offset, top_offset));
    ImGui::SetNextWindowSize(ImVec2(panel_width, panel_height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::Begin("##LeftShadersPanel", nullptr, flags);

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("SHADERS & EFFECTS");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##Filter", "Filter shaders...", m_search_buffer, sizeof(m_search_buffer));
    register_dom_last_item("filter_shaders", "Filter shaders", "input");
    ImGui::Spacing();

    float btn_h = 24.0f;
    float spacing_y = ImGui::GetStyle().ItemSpacing.y;
    float bottom_controls_h = btn_h * 2.0f + spacing_y;
    ImGui::BeginChild("##EffectListRegion", ImVec2(-1.0f, -(bottom_controls_h + spacing_y)), true);

    std::string search_query = m_search_buffer;
    std::transform(search_query.begin(), search_query.end(), search_query.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });

    for (size_t i = 0; i < m_effects.size(); ++i) {
        auto &fx = m_effects[i];

        if (!search_query.empty()) {
            std::string lower_name = fx.name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
            if (lower_name.find(search_query) == std::string::npos) continue;
        }

        ImGui::PushID(static_cast<int>(i));

        bool is_selected = (m_selected_effect_idx == static_cast<int>(i));
        float row_h = 28.0f;
        float avail_w = ImGui::GetContentRegionAvail().x;
        ImVec2 p0 = ImGui::GetCursorScreenPos();

        float margin_x = 4.0f;
        float row_x = p0.x + margin_x;
        float row_w = avail_w - margin_x * 2.0f;
        if (row_w < 60.0f) row_w = 60.0f;

        ImDrawList *draw = ImGui::GetWindowDrawList();
        ImVec2 rect_min(row_x, p0.y + 1.0f);
        ImVec2 rect_max(row_x + row_w, p0.y + row_h - 1.0f);
        bool is_hovered = ImGui::IsMouseHoveringRect(rect_min, rect_max);

        // row background styling
        if (is_selected) {
            draw->AddRectFilled(rect_min, rect_max, IM_COL32(36, 68, 115, 230), 4.0f);
            draw->AddRectFilled(ImVec2(row_x + 1.0f, p0.y + 4.0f), ImVec2(row_x + 4.0f, p0.y + row_h - 4.0f), IM_COL32(70, 160, 255, 255), 2.0f);
        } else if (is_hovered) {
            draw->AddRectFilled(rect_min, rect_max, IM_COL32(42, 45, 52, 180), 4.0f);
        }

        // Vertically centered alignment
        float text_y = p0.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f;
        float check_y = p0.y + (row_h - ImGui::GetFrameHeight()) * 0.5f;

        // dedicated checkbox area
        ImGui::SetCursorScreenPos(ImVec2(row_x + 6.0f, check_y));
        ImGui::Checkbox("##enabled", &fx.enabled);
        register_dom_last_item("shader_checkbox_" + std::to_string(i), fx.name + " Checkbox", "checkbox", fx.enabled);

        // clickable label region for selecting this effect
        ImGui::SetCursorScreenPos(ImVec2(row_x + 30.0f, p0.y + 2.0f));
        float select_w = (std::max)(20.0f, row_w - 34.0f);
        if (ImGui::InvisibleButton("##select_area", ImVec2(select_w, row_h - 4.0f))) {
            m_selected_effect_idx = static_cast<int>(i);
        }
        register_dom_last_item("shader_select_" + std::to_string(i), fx.name, "list_item", is_selected);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            fx.enabled = !fx.enabled;
        }

        // engine badge calculation
        std::string badge;
        if (fx.engine_type.find("ReShade") != std::string::npos) badge = "FX";
        else if (fx.engine_type.find("Unity") != std::string::npos) badge = "Unity";
        else if (fx.engine_type.find("Godot") != std::string::npos) badge = "Godot";
        else badge = "Shader";

        float badge_text_w = ImGui::CalcTextSize(badge.c_str()).x;
        float badge_box_w = badge_text_w + 10.0f;
        bool show_badge = (panel_width >= 210.0f);

        // auto-ellipsis string trimming for shader name
        float max_name_w = row_w - 34.0f - (show_badge ? (badge_box_w + 6.0f) : 4.0f);
        if (max_name_w < 30.0f) max_name_w = 30.0f;

        std::string display_name = fx.name;
        if (ImGui::CalcTextSize(display_name.c_str()).x > max_name_w) {
            while (!display_name.empty() && ImGui::CalcTextSize((display_name + "...").c_str()).x > max_name_w) {
                display_name.pop_back();
            }
            display_name += "...";
        }

        ImU32 text_col = is_selected ? IM_COL32(255, 255, 255, 255) :
                         (fx.enabled ? IM_COL32(230, 230, 235, 255) : IM_COL32(145, 145, 150, 255));
        draw->AddText(ImVec2(row_x + 32.0f, text_y), text_col, display_name.c_str());

        if (show_badge) {
            float badge_h = 16.0f;
            float badge_y = p0.y + (row_h - badge_h) * 0.5f;
            float badge_x = row_x + row_w - badge_box_w - 4.0f;
            draw->AddRectFilled(ImVec2(badge_x, badge_y), ImVec2(badge_x + badge_box_w, badge_y + badge_h), IM_COL32(22, 22, 26, 200), 3.0f);
            ImU32 badge_col = is_selected ? IM_COL32(190, 220, 255, 240) : IM_COL32(120, 120, 130, 200);
            draw->AddText(ImVec2(badge_x + 5.0f, text_y), badge_col, badge.c_str());
        }

        if (is_hovered) {
            ImGui::SetTooltip("%s\nSource: %s\nEngine: %s\nStatus: %s",
                              fx.name.c_str(), fx.file_path.c_str(), fx.engine_type.c_str(),
                              fx.enabled ? "Active" : "Disabled");
        }

        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + row_h + 2.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopID();
    }

    ImGui::EndChild();

    if (ImGui::Button("+ Add Shader", ImVec2(-1.0f, btn_h))) {
        std::wstring path;
        if (open_file_dialog(m_hwnd, L"Shader Files (*.fx;*.shader;*.gdshader;*.hlsl)\0*.fx;*.shader;*.gdshader;*.hlsl\0All Files (*.*)\0*.*\0", path)) {
            m_req_add_shader = true;
            m_add_shader_path = path;
        }
    }
    register_dom_last_item("btn_add_shader", "+ Add Shader", "button");

    float btn_w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    bool can_move_up = (m_selected_effect_idx > 0 && m_selected_effect_idx < static_cast<int>(m_effects.size()));
    if (!can_move_up) ImGui::BeginDisabled();
    if (ImGui::Button("Move Up", ImVec2(btn_w, btn_h))) {
        if (can_move_up) {
            std::swap(m_effects[m_selected_effect_idx], m_effects[m_selected_effect_idx - 1]);
            m_selected_effect_idx--;
        }
    }
    register_dom_last_item("btn_move_up", "Move Up", "button", false, can_move_up);
    if (!can_move_up) ImGui::EndDisabled();

    ImGui::SameLine();

    bool can_move_down = (m_selected_effect_idx >= 0 && m_selected_effect_idx + 1 < static_cast<int>(m_effects.size()));
    if (!can_move_down) ImGui::BeginDisabled();
    if (ImGui::Button("Move Down", ImVec2(btn_w, btn_h))) {
        if (can_move_down) {
            std::swap(m_effects[m_selected_effect_idx], m_effects[m_selected_effect_idx + 1]);
            m_selected_effect_idx++;
        }
    }
    register_dom_last_item("btn_move_down", "Move Down", "button", false, can_move_down);
    if (!can_move_down) ImGui::EndDisabled();

    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(left_offset + panel_width - 0.5f, top_offset),
                  ImVec2(left_offset + panel_width - 0.5f, top_offset + panel_height),
                  m_resizing_left ? IM_COL32(75, 140, 220, 240) : IM_COL32(36, 36, 40, 220), m_resizing_left ? 2.0f : 1.0f);

    ImGui::End();
    ImGui::PopStyleVar();
}

void CustomUI::render_center_canvas(float left_offset, float top_offset, float canvas_width, float canvas_height,
                                    uint32_t img_w, uint32_t img_h,
                                    float /*zoom_level*/, float /*pan_x*/, float /*pan_y*/) {
    ImDrawList *fg = ImGui::GetBackgroundDrawList();

    // 1. splitter lines on left and right borders of canvas are drawn directly in panel windows
    // to prevent popup occlusion and ensure active highlight lines are always visible.

    // 2. before / after split divider line & handle
    if (m_active_tool == ActiveTool::Split && m_before_after && img_w > 0 && img_h > 0) {
        float split_norm = std::clamp(0.5f + m_before_after_split, 0.05f, 0.95f);
        float split_x = left_offset + canvas_width * split_norm;
        float canvas_top = top_offset;
        float canvas_bot = top_offset + canvas_height;

        fg->AddLine(ImVec2(split_x, canvas_top), ImVec2(split_x, canvas_bot), IM_COL32(255, 255, 255, 210), 2.0f);

        float handle_y = canvas_top + canvas_height * 0.5f;
        fg->AddCircleFilled(ImVec2(split_x, handle_y), 12.0f, IM_COL32(28, 28, 30, 240));
        fg->AddCircle(ImVec2(split_x, handle_y), 12.0f, IM_COL32(255, 255, 255, 220), 24, 1.5f);
        fg->AddText(ImVec2(split_x - 11.0f, handle_y - 7.0f), IM_COL32(230, 230, 230, 255), "< | >");
    }

    // 3. erase brush mask strokes (projected from image UV coords)
    bool show_mask = (m_active_tool == ActiveTool::Erase) || (!m_erase_strokes.empty() && m_active_tool == ActiveTool::Hand);
    if (show_mask && img_w > 0 && img_h > 0) {
        float fit_scale = (img_w > 0 && img_h > 0) ?
            (std::min)(canvas_width / static_cast<float>(img_w), canvas_height / static_cast<float>(img_h)) : 1.0f;
        float eff_zoom = m_last_zoom * fit_scale;
        float cx = left_offset + canvas_width * 0.5f;
        float cy = top_offset + canvas_height * 0.5f;

        auto uv_to_screen = [&](const ImVec2 &uv) -> ImVec2 {
            float qx = (uv.x * img_w) - (img_w * 0.5f) - m_last_pan_x;
            float qy = (uv.y * img_h) - (img_h * 0.5f) - m_last_pan_y;
            return ImVec2(cx + qx * eff_zoom, cy + qy * eff_zoom);
        };

        // Strict clipping bounds: intersection of canvas viewable rect and projected image rect
        ImVec2 p_img_tl = uv_to_screen(ImVec2(0.0f, 0.0f));
        ImVec2 p_img_br = uv_to_screen(ImVec2(1.0f, 1.0f));

        float clip_x0 = (std::max)(left_offset, (std::min)(p_img_tl.x, p_img_br.x));
        float clip_y0 = (std::max)(top_offset, (std::min)(p_img_tl.y, p_img_br.y));
        float clip_x1 = (std::min)(left_offset + canvas_width, (std::max)(p_img_tl.x, p_img_br.x));
        float clip_y1 = (std::min)(top_offset + canvas_height, (std::max)(p_img_tl.y, p_img_br.y));

        if (clip_x1 > clip_x0 && clip_y1 > clip_y0) {
            fg->PushClipRect(ImVec2(clip_x0, clip_y0), ImVec2(clip_x1, clip_y1), true);

            float screen_radius = m_erase_radius * eff_zoom;
            if (screen_radius < 2.0f) screen_radius = 2.0f;

            ImU32 stroke_col = (m_active_tool == ActiveTool::Erase) ? IM_COL32(235, 45, 45, 130) : IM_COL32(235, 45, 45, 75);

            auto draw_stroke_smooth = [&](const std::vector<ImVec2> &pts) {
                if (pts.empty()) return;
                if (pts.size() == 1) {
                    ImVec2 sp = uv_to_screen(pts[0]);
                    fg->AddCircleFilled(sp, screen_radius, stroke_col, 20);
                    return;
                }
                for (size_t s = 1; s < pts.size(); ++s) {
                    ImVec2 p_a = uv_to_screen(pts[s - 1]);
                    ImVec2 p_b = uv_to_screen(pts[s]);
                    fg->AddLine(p_a, p_b, stroke_col, screen_radius * 2.0f);
                }
                fg->AddCircleFilled(uv_to_screen(pts.front()), screen_radius, stroke_col, 20);
                fg->AddCircleFilled(uv_to_screen(pts.back()), screen_radius, stroke_col, 20);
            };

            for (const auto &stroke : m_erase_strokes) {
                draw_stroke_smooth(stroke);
            }
            if (!m_current_stroke.empty()) {
                draw_stroke_smooth(m_current_stroke);
            }

            fg->PopClipRect();
        }

        // Cursor ring: exact size match to painted stroke
        if (m_active_tool == ActiveTool::Erase) {
            POINT mouse_pt;
            GetCursorPos(&mouse_pt);
            ScreenToClient(m_hwnd, &mouse_pt);
            if (mouse_pt.x >= left_offset && mouse_pt.x <= left_offset + canvas_width &&
                mouse_pt.y >= top_offset && mouse_pt.y <= top_offset + canvas_height) {
                bool alt_held = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                bool is_sub = m_erase_subtract || alt_held;

                float screen_radius = m_erase_radius * eff_zoom;
                if (screen_radius < 3.0f) screen_radius = 3.0f;

                ImVec2 mpos(static_cast<float>(mouse_pt.x), static_cast<float>(mouse_pt.y));
                ImU32 ring_col = is_sub ? IM_COL32(255, 200, 50, 230) : IM_COL32(255, 255, 255, 220);

                fg->AddCircle(mpos, screen_radius, ring_col, 32, 1.5f);
                if (is_sub) {
                    fg->AddLine(ImVec2(mpos.x - screen_radius * 0.45f, mpos.y),
                                ImVec2(mpos.x + screen_radius * 0.45f, mpos.y),
                                IM_COL32(255, 200, 50, 240), 1.5f);
                } else {
                    fg->AddCircle(mpos, 1.5f, IM_COL32(255, 255, 255, 240), 10, 1.0f);
                }
            }
        }
    }

    // 4. centered empty state when no image loaded
    if (img_w == 0 || img_h == 0) {
        float box_w = (std::min)(canvas_width - 40.0f, 440.0f);
        float box_h = 152.0f;
        float box_x = left_offset + (canvas_width - box_w) * 0.5f;
        float box_y = top_offset + (canvas_height - box_h) * 0.44f;

        ImGui::SetNextWindowPos(ImVec2(box_x, box_y));
        ImGui::SetNextWindowSize(ImVec2(box_w, box_h));
        ImGui::Begin("##EmptyStateBox", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
        float title_w = ImGui::CalcTextSize("ShaderLab").x;
        ImGui::SetCursorPosX((box_w - title_w) * 0.5f);
        ImGui::TextUnformatted("ShaderLab");
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.70f, 1.0f));
        const char *subtitle = "Drop an image or project file to begin editing";
        float sub_w = ImGui::CalcTextSize(subtitle).x;
        ImGui::SetCursorPosX((box_w - sub_w) * 0.5f);
        ImGui::TextUnformatted(subtitle);
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.48f, 1.0f));
        const char *formats = "Supports PNG, JPG, JPEG, BMP, TGA, .sldepth";
        float fmt_w = ImGui::CalcTextSize(formats).x;
        ImGui::SetCursorPosX((box_w - fmt_w) * 0.5f);
        ImGui::TextUnformatted(formats);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float btn_w = 200.0f;
        ImGui::SetCursorPosX((box_w - btn_w) * 0.5f);
        if (ImGui::Button("Open Image... (Ctrl+O)", ImVec2(btn_w, 28.0f))) {
            std::wstring path;
            if (open_file_dialog(m_hwnd, L"Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files (*.*)\0*.*\0", path)) {
                m_req_open_image = true;
                m_open_image_path = path;
            }
        }
        register_dom_last_item("empty_btn_open", "Open Image...", "button");

        ImGui::End();
    }
}

void CustomUI::render_right_panel(float right_offset, float top_offset, float panel_width, float panel_height) {
    ImGui::SetNextWindowPos(ImVec2(right_offset, top_offset));
    ImGui::SetNextWindowSize(ImVec2(panel_width, panel_height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::Begin("##RightSettingsPanel", nullptr, flags);

    ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_FittingPolicyResizeDown;
    if (ImGui::BeginTabBar("##RightPanelTabs", tab_bar_flags)) {
        // --- TAB 1: SHADERS ---
        ImGuiTabItemFlags tab0_flags = (m_sync_tab_req == 0) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("Shaders", nullptr, tab0_flags)) {
            if (m_sync_tab_req < 0 && m_right_panel_tab != 0) {
                m_right_panel_tab = 0;
                if (m_active_tool == ActiveTool::Depth || m_active_tool == ActiveTool::Erase) {
                    set_active_tool(ActiveTool::Hand);
                }
            }
            register_dom_last_item("tab_shaders", "Shaders Tab", "tab", true);

            float reset_btn_h = 26.0f;
            float spacing_y = ImGui::GetStyle().ItemSpacing.y;

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.0f, 4.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
            ImGui::BeginChild("##ShadersUniformScroll", ImVec2(0.0f, -(reset_btn_h + spacing_y + 4.0f)), false);

            if (m_selected_effect_idx >= 0 && m_selected_effect_idx < static_cast<int>(m_effects.size())) {
                auto &fx = m_effects[m_selected_effect_idx];

                float wrap_w = ImGui::GetContentRegionAvail().x - 12.0f;
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
                ImGui::Text("Effect: %s", fx.name.c_str());
                ImGui::TextDisabled("Source: %s (%s)", fx.file_path.c_str(), fx.engine_type.c_str());
                ImGui::PopTextWrapPos();

                ImGui::Spacing();
                ImGui::Checkbox("Enable Effect", &fx.enabled);
                ImGui::Separator();

                std::string current_category = "";
                for (auto &u : fx.uniforms) {
                    if (!u.category.empty() && u.category != current_category) {
                        current_category = u.category;
                        ImGui::Spacing();
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
                        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "[ %s ]", current_category.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::Separator();
                    }

                    ImGui::PushID(u.name.c_str());

                    if (u.type == ShaderLabUniform::Type::Bool) {
                        ImGui::Checkbox(u.label.c_str(), &u.bool_val);
                        if (ImGui::IsItemHovered() && !u.tooltip.empty()) {
                            ImGui::SetTooltip("%s", u.tooltip.c_str());
                        }
                    } else {
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
                        ImGui::Text("%s", u.label.c_str());
                        ImGui::PopTextWrapPos();
                        if (ImGui::IsItemHovered() && !u.tooltip.empty()) {
                            ImGui::SetTooltip("%s", u.tooltip.c_str());
                        }

                        switch (u.type) {
                        case ShaderLabUniform::Type::Float: {
                            ImGui::SetNextItemWidth(-12.0f);
                            ImGui::SliderFloat("##val", &u.float_val, u.min_val, u.max_val, "%.3f");
                            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                                u.float_val = u.default_float;
                            }
                            break;
                        }
                        case ShaderLabUniform::Type::Int: {
                            ImGui::SetNextItemWidth(-12.0f);
                            ImGui::SliderInt("##val", &u.int_val, static_cast<int>(u.min_val), static_cast<int>(u.max_val));
                            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                                u.int_val = u.default_int;
                            }
                            break;
                        }
                        case ShaderLabUniform::Type::Color3: {
                            ImGui::SetNextItemWidth(-12.0f);
                            ImGuiColorEditFlags color_flags = (panel_width < 290.0f) ?
                                (ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha) : 0;
                            ImGui::ColorEdit3("##val", u.color_val, color_flags);
                            break;
                        }
                        case ShaderLabUniform::Type::Color4: {
                            ImGui::SetNextItemWidth(-12.0f);
                            ImGuiColorEditFlags color_flags = (panel_width < 290.0f) ?
                                (ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar) : ImGuiColorEditFlags_AlphaBar;
                            ImGui::ColorEdit4("##val", u.color_val, color_flags);
                            break;
                        }
                        case ShaderLabUniform::Type::Enum: {
                            ImGui::SetNextItemWidth(-12.0f);
                            if (ImGui::BeginCombo("##val", u.enum_selected < static_cast<int>(u.enum_items.size()) ?
                                                            u.enum_items[u.enum_selected].c_str() : "")) {
                                for (int opt = 0; opt < static_cast<int>(u.enum_items.size()); ++opt) {
                                    bool sel = (u.enum_selected == opt);
                                    if (ImGui::Selectable(u.enum_items[opt].c_str(), sel)) {
                                        u.enum_selected = opt;
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            break;
                        }
                        default:
                            break;
                        }
                    }

                    ImGui::PopID();
                    ImGui::Spacing();
                }
            } else {
                float wrap_w = ImGui::GetContentRegionAvail().x - 12.0f;
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
                ImGui::TextDisabled("Select an effect on the left to configure its parameters.");
                ImGui::PopTextWrapPos();
            }

            ImGui::EndChild();
            ImGui::PopStyleVar(2);

            // Pinned Reset button at bottom of Shaders tab with clean margins
            bool has_effect = (m_selected_effect_idx >= 0 && m_selected_effect_idx < static_cast<int>(m_effects.size()));
            if (!has_effect) ImGui::BeginDisabled();
            float avail_w = ImGui::GetContentRegionAvail().x;
            if (ImGui::Button("Reset Parameters to Default", ImVec2(avail_w - 12.0f, reset_btn_h))) {
                if (has_effect) m_effects[m_selected_effect_idx].reset_uniforms();
            }
            register_dom_last_item("btn_reset_defaults", "Reset Parameters to Default", "button", false, has_effect);
            if (!has_effect) ImGui::EndDisabled();

            ImGui::EndTabItem();
        } else {
            register_dom_last_item("tab_shaders", "Shaders Tab", "tab", false);
        }

        // --- TAB 2: 3D DEPTH ---
        ImGuiTabItemFlags tab1_flags = (m_sync_tab_req == 1) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("3D Depth", nullptr, tab1_flags)) {
            if (m_sync_tab_req < 0 && m_right_panel_tab != 1) {
                m_right_panel_tab = 1;
                set_active_tool(ActiveTool::Depth);
            }
            register_dom_last_item("tab_depth", "3D Depth Tab", "tab", true);
            register_dom_last_item("accordion_depth", "3D Depth Tab", "tab", true);

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.0f, 4.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
            ImGui::BeginChild("##DepthPanelScroll", ImVec2(0.0f, 0.0f), false);

            if (m_has_depth) {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Status: 3D Depth Active (%ux%u)", m_depth_w, m_depth_h);
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Status: No 3D Depth Attached");
            }
            ImGui::Spacing();

            bool depth_peek = (m_active_tool == ActiveTool::Depth && m_depth_peek);
            if (ImGui::Checkbox("Live Depth Peek (Hold D or Click)", &depth_peek)) {
                set_active_tool(depth_peek ? ActiveTool::Depth : ActiveTool::Hand);
            }
            register_dom_last_item("depth_checkbox_peek", "Live Depth Peek", "checkbox", depth_peek);

            ImGui::Checkbox("Invert Depth Buffer", &m_depth_invert);
            register_dom_last_item("depth_checkbox_invert", "Invert Depth Buffer", "checkbox", m_depth_invert);
            ImGui::Spacing();

            float wrap_w = ImGui::GetContentRegionAvail().x - 12.0f;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
            ImGui::TextUnformatted("Far Clipping Plane");
            ImGui::PopTextWrapPos();
            ImGui::SetNextItemWidth(-12.0f);
            ImGui::SliderFloat("##FarPlane", &m_depth_far_plane, 10.0f, 5000.0f, "%.0f");
            register_dom_last_item("depth_slider_far", "Far Clipping Plane", "slider");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adjusts far depth distance calibration");
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) m_depth_far_plane = 1000.0f;
            ImGui::Spacing();

            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
            ImGui::TextUnformatted("Machine Learning Depth Estimation");
            ImGui::PopTextWrapPos();
            ImGui::SetNextItemWidth(-12.0f);
            const char *models[] = { "Large (~1.3GB ViT-L)", "Base (~390MB ViT-B)", "Small (~100MB ViT-S)" };
            static int selected_model = 0;
            if (ImGui::Combo("##AIModel", &selected_model, models, 3)) {
                if (selected_model == 0) m_depth_encoder = "vitl";
                else if (selected_model == 1) m_depth_encoder = "vitb";
                else m_depth_encoder = "vits";
            }
            register_dom_last_item("depth_combo_model", "Machine Learning Depth Estimation", "combo");

            if (m_depth_is_busy) {
                ImGui::BeginDisabled();
                ImGui::Button(m_depth_status_text.empty() ? "Generating Depth..." : m_depth_status_text.c_str(), ImVec2(-12.0f, 28.0f));
                ImGui::EndDisabled();
            } else {
                if (ImGui::Button("Generate 3D Depth Map", ImVec2(-12.0f, 28.0f))) {
                    m_req_generate_depth = true;
                    m_depth_status_text = "Running Depth-Anything-V2 (" + m_depth_encoder + ")...";
                    m_depth_is_busy = true;
                }
                register_dom_last_item("depth_btn_generate", "Generate 3D Depth Map", "button");
            }
            if (!m_depth_status_text.empty() && !m_depth_is_busy) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", m_depth_status_text.c_str());
                ImGui::PopTextWrapPos();
            }

            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::EndTabItem();
        } else {
            register_dom_last_item("tab_depth", "3D Depth Tab", "tab", false);
            register_dom_last_item("accordion_depth", "3D Depth Tab", "tab", false);
        }

        // --- TAB 3: ERASE & INPAINT ---
        ImGuiTabItemFlags tab2_flags = (m_sync_tab_req == 2) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("Erase & Inpaint", nullptr, tab2_flags)) {
            if (m_sync_tab_req < 0 && m_right_panel_tab != 2) {
                m_right_panel_tab = 2;
                set_active_tool(ActiveTool::Erase);
            }
            register_dom_last_item("tab_erase", "Erase Tab", "tab", true);
            register_dom_last_item("accordion_erase", "Erase Tab", "tab", true);

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.0f, 4.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
            ImGui::BeginChild("##ErasePanelScroll", ImVec2(0.0f, 0.0f), false);

            bool erase_active = (m_active_tool == ActiveTool::Erase && m_erase_active);
            if (ImGui::Checkbox("Activate Erase Brush (E)", &erase_active)) {
                set_active_tool(erase_active ? ActiveTool::Erase : ActiveTool::Hand);
            }
            register_dom_last_item("erase_checkbox_activate", "Activate Erase Brush", "checkbox", erase_active);
            ImGui::Spacing();

            float wrap_w = ImGui::GetContentRegionAvail().x - 12.0f;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
            ImGui::TextUnformatted("Brush Radius ( [ / ] )");
            ImGui::PopTextWrapPos();
            ImGui::SetNextItemWidth(-12.0f);
            ImGui::SliderFloat("##Radius", &m_erase_radius, 4.0f, 128.0f, "%.0f px");
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) m_erase_radius = 24.0f;
            register_dom_last_item("erase_slider_radius", "Brush Radius", "slider");

            ImGui::Checkbox("Subtract Mask Mode (Alt)", &m_erase_subtract);
            register_dom_last_item("erase_checkbox_subtract", "Subtract Mask Mode", "checkbox", m_erase_subtract);
            ImGui::Spacing();

            float half_btn = (ImGui::GetContentRegionAvail().x - 12.0f - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button("Undo Stroke (Ctrl+Z)", ImVec2(half_btn, 24.0f))) {
                undo_erase_stroke();
            }
            register_dom_last_item("erase_btn_undo", "Undo Stroke", "button");
            ImGui::SameLine();
            if (ImGui::Button("Clear Mask (Esc)", ImVec2(half_btn, 24.0f))) {
                clear_erase_strokes();
                m_req_clear_mask = true;
            }
            register_dom_last_item("erase_btn_clear", "Clear Mask", "button");
            ImGui::Spacing();

            if (m_inpaint_is_busy) {
                ImGui::BeginDisabled();
                ImGui::Button(m_inpaint_status_text.empty() ? "Inpainting..." : m_inpaint_status_text.c_str(), ImVec2(-12.0f, 28.0f));
                ImGui::EndDisabled();
            } else {
                bool has_mask = !m_erase_strokes.empty();
                if (!has_mask) ImGui::BeginDisabled();
                if (ImGui::Button("Inpaint Selection (LaMa)", ImVec2(-12.0f, 28.0f))) {
                    m_req_inpaint = true;
                    m_inpaint_status_text = "Running LaMa Big HD inpainting...";
                    m_inpaint_is_busy = true;
                }
                register_dom_last_item("erase_btn_inpaint", "Inpaint Selection", "button");
                if (!has_mask) ImGui::EndDisabled();
            }
            if (!m_inpaint_status_text.empty() && !m_inpaint_is_busy) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", m_inpaint_status_text.c_str());
                ImGui::PopTextWrapPos();
            }

            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::EndTabItem();
        } else {
            register_dom_last_item("tab_erase", "Erase Tab", "tab", false);
            register_dom_last_item("accordion_erase", "Erase Tab", "tab", false);
        }

        m_sync_tab_req = -1;
        ImGui::EndTabBar();
    }

    // Left border drawn in panel window draw list
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(right_offset + 0.5f, top_offset),
                  ImVec2(right_offset + 0.5f, top_offset + panel_height),
                  m_resizing_right ? IM_COL32(75, 140, 220, 240) : IM_COL32(36, 36, 40, 220), m_resizing_right ? 2.0f : 1.0f);

    ImGui::End();
    ImGui::PopStyleVar();
}

void CustomUI::render_bottom_status_bar(float top_offset, float width, float height,
                                       uint32_t img_w, uint32_t img_h,
                                       float zoom_level, float pan_x, float pan_y) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, top_offset));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                            ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 3.0f));
    ImGui::Begin("##PhotoshopStatusBar", nullptr, flags);

    ImGui::AlignTextToFramePadding();
    if (img_w > 0 && img_h > 0) {
        float canvas_w = (std::max)(64.0f, width - get_toolbar_width() - get_left_panel_width(static_cast<uint32_t>(width)) - get_right_panel_width(static_cast<uint32_t>(width)));
        float canvas_h = (std::max)(64.0f, m_canvas_view_h > 0.0f ? m_canvas_view_h : 600.0f);
        float fit_scale = (img_w > 0 && img_h > 0) ? (std::min)(canvas_w / static_cast<float>(img_w), canvas_h / static_cast<float>(img_h)) : 1.0f;
        float true_percent = zoom_level * fit_scale * 100.0f;
        ImGui::Text("Canvas: %ux%u | Zoom: %.0f%% | Pan: (%.0f, %.0f)",
                    img_w, img_h, true_percent, pan_x, pan_y);

        if (m_master_bypass) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "[Bypass: Raw Original]");
        }
        if (m_active_tool == ActiveTool::Depth && m_depth_peek) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[Depth Peek]");
        }
        if (m_active_tool == ActiveTool::Split && m_before_after) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "[Split]");
        }
        if (m_active_tool == ActiveTool::Erase && m_erase_active) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "[Erase Brush]");
        }
    } else {
        ImGui::TextDisabled("No image loaded");
    }

    float right_area_w = 320.0f;
    if (width > right_area_w + 350.0f) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(width - right_area_w);
        if (ImGui::SmallButton("Fit")) {
            m_req_fit_view = true;
        }
        register_dom_last_item("status_btn_fit", "Fit", "button");
        register_dom_last_item("opt_fit", "Fit", "button");
        ImGui::SameLine();
        if (ImGui::SmallButton("100%")) {
            m_req_100_view = true;
        }
        register_dom_last_item("status_btn_100", "100%", "button");
        register_dom_last_item("opt_100", "100%", "button");
        ImGui::SameLine();
        if (ImGui::SmallButton("200%")) {
            m_req_200_view = true;
        }
        register_dom_last_item("status_btn_200", "200%", "button");
        register_dom_last_item("opt_200", "200%", "button");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("[Tab: Hide Panels]");
    }

    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(0.0f, top_offset + 0.5f),
                  ImVec2(width, top_offset + 0.5f),
                  IM_COL32(36, 36, 40, 220), 1.0f);

    ImGui::End();
    ImGui::PopStyleVar();
}

void CustomUI::render(uint32_t screen_w, uint32_t screen_h,
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

    float body_y = menu_height + tool_options_h;
    float body_h = total_h - body_y - status_bar_h;
    if (body_h < 64.0f) body_h = 64.0f;

    float viewable_x = toolbar_w + left_w;
    float viewable_w = total_w - viewable_x - right_w;
    if (viewable_w < 64.0f) viewable_w = 64.0f;

    m_canvas_view_x = viewable_x;
    m_canvas_view_y = body_y;
    m_canvas_view_w = viewable_w;
    m_canvas_view_h = body_h;
    m_last_img_w = img_w;
    m_last_img_h = img_h;
    m_last_zoom = zoom_level;
    m_last_pan_x = pan_x;
    m_last_pan_y = pan_y;

    render_tool_options_bar(menu_height, total_w);
    render_tool_strip(body_y, body_h);

    if (!m_hide_panels && left_w > 0.0f) {
        render_left_panel(toolbar_w, body_y, left_w, body_h);
    }

    render_center_canvas(viewable_x, body_y, viewable_w, body_h, img_w, img_h, zoom_level, pan_x, pan_y);

    if (!m_hide_panels && right_w > 0.0f) {
        render_right_panel(total_w - right_w, body_y, right_w, body_h);
    }

    render_bottom_status_bar(total_h - status_bar_h, total_w, status_bar_h, img_w, img_h, zoom_level, pan_x, pan_y);

    render_menu_bar();

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

void CustomUI::end_frame() {
    if (!m_initialized) return;
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void CustomUI::simulate_canvas_mouse(float x, float y, int act) {
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
