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

void GuiManager::set_active_tool(ActiveTool tool) {
    m_active_tool = tool;
    m_erase_active = (tool == ActiveTool::Erase);
    m_before_after = (tool == ActiveTool::Split);
    m_depth_peek   = (tool == ActiveTool::Depth);
    if (tool == ActiveTool::Erase) {
        m_right_panel_tab = 2;
    } else if (tool == ActiveTool::Depth) {
        m_right_panel_tab = 1;
    } else if (tool == ActiveTool::Hand) {
        m_right_panel_tab = 0;
    }
    EraseTool &erase_tool = EraseTool::get();
    erase_tool.set_active(tool == ActiveTool::Erase);
    if (tool == ActiveTool::Erase) {
        erase_tool.brush_radius = m_erase_radius;
    }
}

void GuiManager::select_right_panel_tab(int tab_idx) {
    m_right_panel_tab = tab_idx;
    if (tab_idx == 0) {
        if (m_active_tool == ActiveTool::Erase || m_active_tool == ActiveTool::Depth) {
            set_active_tool(ActiveTool::Hand);
        }
    } else if (tab_idx == 1) {
        set_active_tool(ActiveTool::Depth);
    } else if (tab_idx == 2) {
        set_active_tool(ActiveTool::Erase);
    }
}

void GuiManager::request_inpaint() {
    if (EraseTool::get().is_processing() || m_inpaint_is_busy) return;
    if (!EraseTool::get().has_mask_edits() && m_erase_strokes.empty()) return;
    m_req_inpaint = true;
    m_inpaint_status_text = "Running LaMa Big HD inpainting...";
    m_inpaint_is_busy = true;
}

void GuiManager::on_erase_stroke_finished(bool was_subtract) {
    if (!was_subtract && EraseTool::get().auto_apply && !EraseTool::get().is_processing() && EraseTool::get().has_mask_edits()) {
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
    bool tool_undone = EraseTool::get().undo_stroke();
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
    bool tool_redone = EraseTool::get().redo_stroke();
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
            tool.draw_brush_dab((int)std::round(prev.x), (int)std::round(prev.y), (int)std::round(m_erase_radius), 255);
            for (size_t i = 1; i < stroke.size(); ++i) {
                ImVec2 curr(stroke[i].x * static_cast<float>(img_w), stroke[i].y * static_cast<float>(img_h));
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

GuiManager::CursorHitState GuiManager::compute_cursor_hit_state(POINT pt) const {
    CursorHitState hit{};
    RECT client_rc = {};
    GetClientRect(m_hwnd, &client_rc);
    hit.screen_w = static_cast<float>(client_rc.right - client_rc.left);
    float screen_h = static_cast<float>(client_rc.bottom - client_rc.top);
    float menu_h = get_menu_bar_height();
    float tool_options_h = get_tool_options_height();
    float toolbar_w = get_toolbar_width();
    float status_bar_h = get_status_bar_height();

    float left_w = get_left_panel_width(static_cast<uint32_t>(hit.screen_w));
    float right_w = get_right_panel_width(static_cast<uint32_t>(hit.screen_w));
    hit.viewable_x = toolbar_w + left_w;
    hit.viewable_y = menu_h + tool_options_h;
    hit.viewable_w = hit.screen_w - hit.viewable_x - right_w;
    hit.viewable_h = screen_h - hit.viewable_y - status_bar_h;

    float left_splitter_x = toolbar_w + left_w;
    float right_splitter_x = hit.screen_w - right_w;
    bool in_vert = (pt.y >= hit.viewable_y && pt.y < hit.viewable_y + hit.viewable_h);

    hit.near_left_splitter = (!m_hide_panels && in_vert && std::abs(static_cast<float>(pt.x) - left_splitter_x) <= 5.0f);
    hit.near_right_splitter = (!m_hide_panels && in_vert && std::abs(static_cast<float>(pt.x) - right_splitter_x) <= 5.0f);

    float split_norm = std::clamp(0.5f + m_before_after_split, 0.05f, 0.95f);
    float split_x = hit.viewable_x + hit.viewable_w * split_norm;
    hit.near_split_line = m_before_after && in_vert && (std::abs(static_cast<float>(pt.x) - split_x) <= 8.0f);

    hit.in_canvas = (pt.x > hit.viewable_x + 4.0f && pt.x < (hit.viewable_x + hit.viewable_w) - 4.0f && in_vert);
    hit.popup_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    hit.in_client = PtInRect(&client_rc, pt) != FALSE;
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

    if (m_is_painting_mask) {
        SetCursor(nullptr);
        m_active_cursor_name = "none";
        return;
    }

    if (!hit.popup_open && (hit.near_left_splitter || hit.near_right_splitter || hit.near_split_line)) {
        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
        m_active_cursor_name = "sizewe";
        return;
    }

    bool space_held = m_space_held || ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0);
    if (hit.in_canvas && has_loaded_image() && !hit.popup_open) {
        bool want_text = ImGui::GetIO().WantTextInput;
        if (m_active_tool == ActiveTool::Hand || (space_held && !want_text)) {
            HINSTANCE hInst = GetModuleHandleW(nullptr);
            if (!m_cursor_grab) m_cursor_grab = LoadCursorW(hInst, MAKEINTRESOURCEW(IDC_CURSOR_GRAB));
            if (!m_cursor_grip) m_cursor_grip = LoadCursorW(hInst, MAKEINTRESOURCEW(IDC_CURSOR_GRIP));
            SetCursor(m_cursor_grab ? m_cursor_grab : LoadCursor(nullptr, IDC_HAND));
            m_active_cursor_name = "grab";
            return;
        }
        if (m_active_tool == ActiveTool::Erase) {
            // erase tool hides OS cursor on canvas so custom brush ring draws cleanly
            SetCursor(nullptr);
            m_active_cursor_name = "none";
            return;
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
    m_last_paint_px = ImVec2(-1.0f, -1.0f);
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

    // update pointer coordinate only from real mouse msgs or initial entry, never clobber on keydown
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

    CursorHitState hit = self->compute_cursor_hit_state(pt);
    bool active_drag = self->has_active_drag();

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

    // keyboard routing: single owner for Space pan and hotkeys before imgui/viewport
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
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (!ctrl) {
                self->m_depth_key_held = true;
                // transient depth hold peek: do not toggle tools or switch tabs
                return true;
            }
        }

        if (wParam == VK_SPACE) {
            ImGui_ImplWin32_WndProcHandler(self->m_hwnd, msg, wParam, lParam);
            if (!ImGui::GetIO().WantTextInput && !hit.popup_open) {
                self->m_space_held = true;
                self->vp_set_space_pan(true);
                sync_hover_cursor();
                // swallow space when hovering/dragging canvas or outside text input so focused toolbar buttons dont click
                return true;
            }
            return true;
        }

        if (ImGui_ImplWin32_WndProcHandler(self->m_hwnd, msg, wParam, lParam)) {
            return true;
        }

        const ImGuiIO &io = ImGui::GetIO();
        if (msg == WM_KEYDOWN && !io.WantTextInput && !hit.popup_open) {
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            if (wParam == VK_TAB) {
                self->toggle_panels_hidden();
                sync_hover_cursor();
                return true;
            } else if (wParam == 'H' && !ctrl) {
                self->set_active_tool(ActiveTool::Hand);
                sync_hover_cursor();
                return true;
            } else if (wParam == 'E' && !ctrl) {
                self->set_active_tool(self->m_active_tool == ActiveTool::Erase ? ActiveTool::Hand : ActiveTool::Erase);
                sync_hover_cursor();
                return true;
            } else if (wParam == 'B' && !ctrl) {
                self->set_active_tool(self->m_active_tool == ActiveTool::Split ? ActiveTool::Hand : ActiveTool::Split);
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
                self->m_selected_effect_idx = 0;
                return true;
            } else if (wParam == VK_OEM_4) {
                self->m_erase_radius = (std::max)(2.0f, self->m_erase_radius - 4.0f);
                EraseTool::get().brush_radius = self->m_erase_radius;
                return true;
            } else if (wParam == VK_OEM_6) {
                self->m_erase_radius = (std::min)(256.0f, self->m_erase_radius + 4.0f);
                EraseTool::get().brush_radius = self->m_erase_radius;
                return true;
            } else if (wParam == 'Z' && ctrl && !shift &&
                       (self->m_active_tool == ActiveTool::Erase || EraseTool::get().can_undo_stroke())) {
                self->undo_erase_stroke();
                return true;
            } else if (((wParam == 'Y' && ctrl) || (wParam == 'Z' && ctrl && shift)) &&
                       (self->m_active_tool == ActiveTool::Erase || EraseTool::get().can_redo_stroke())) {
                self->redo_erase_stroke();
                return true;
            } else if (wParam == VK_RETURN && !ctrl && !shift && !alt &&
                       self->m_active_tool == ActiveTool::Erase &&
                       EraseTool::get().has_mask_edits() &&
                       !EraseTool::get().is_processing()) {
                self->request_inpaint();
                return true;
            } else if (wParam == VK_ESCAPE) {
                bool has_mask = !self->m_erase_strokes.empty() || EraseTool::get().has_mask_edits();
                if (self->m_active_tool == ActiveTool::Erase && has_mask) {
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

    // continue active UI or canvas drags regardless of cursor position
    if (self->m_resizing_left || self->m_resizing_right || self->m_is_dragging_split) {
        if (!is_mouse_msg) return false;
        if (msg == WM_MOUSEMOVE) {
            if (self->m_resizing_left) {
                float new_w = static_cast<float>(pt.x) - self->get_toolbar_width();
                self->m_left_panel_width = std::clamp(new_w, 240.0f, hit.screen_w * 0.45f);
            } else if (self->m_resizing_right) {
                float new_w = hit.screen_w - static_cast<float>(pt.x);
                self->m_right_panel_width = std::clamp(new_w, 300.0f, hit.screen_w * 0.45f);
            } else if (self->m_is_dragging_split) {
                float rel_x = (static_cast<float>(pt.x) - hit.viewable_x) / (hit.viewable_w > 0.0f ? hit.viewable_w : 1.0f);
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
        float fit_scale = (self->m_last_img_w > 0 && self->m_last_img_h > 0) ?
            (std::min)(self->m_canvas_view_w / static_cast<float>(self->m_last_img_w),
                       self->m_canvas_view_h / static_cast<float>(self->m_last_img_h)) : 1.0f;
        float eff_zoom = self->m_last_zoom * fit_scale;
        if (eff_zoom <= 0.001f) eff_zoom = 1.0f;

        float cx = self->m_canvas_view_x + self->m_canvas_view_w * 0.5f;
        float cy = self->m_canvas_view_y + self->m_canvas_view_h * 0.5f;
        float img_px_x = (static_cast<float>(pt.x) - cx) / eff_zoom + self->m_last_pan_x + (self->m_last_img_w * 0.5f);
        float img_px_y = (static_cast<float>(pt.y) - cy) / eff_zoom + self->m_last_pan_y + (self->m_last_img_h * 0.5f);

        float margin_px = self->m_erase_radius + 4.0f;
        over_image = (self->m_last_img_w > 0 && self->m_last_img_h > 0 &&
                      img_px_x >= -margin_px && img_px_x <= self->m_last_img_w + margin_px &&
                      img_px_y >= -margin_px && img_px_y <= self->m_last_img_h + margin_px);

        float clamped_px_x = std::clamp(img_px_x, 0.0f, static_cast<float>(self->m_last_img_w));
        float clamped_px_y = std::clamp(img_px_y, 0.0f, static_cast<float>(self->m_last_img_h));
        uv_x = (self->m_last_img_w > 0) ? (clamped_px_x / static_cast<float>(self->m_last_img_w)) : 0.0f;
        uv_y = (self->m_last_img_h > 0) ? (clamped_px_y / static_cast<float>(self->m_last_img_h)) : 0.0f;
        rad_uv_x = (self->m_last_img_w > 0) ? (self->m_erase_radius / static_cast<float>(self->m_last_img_w)) : 0.01f;
        rad_uv_y = (self->m_last_img_h > 0) ? (self->m_erase_radius / static_cast<float>(self->m_last_img_h)) : 0.01f;
    };

    if (self->m_is_painting_mask) {
        if (!is_mouse_msg) return false;
        float uv_x = 0.0f, uv_y = 0.0f, rad_uv_x = 0.01f, rad_uv_y = 0.01f;
        bool over_image = false;
        compute_brush_uv(uv_x, uv_y, rad_uv_x, rad_uv_y, over_image);
        bool is_sub = self->m_erase_subtract || ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);

        if (msg == WM_MOUSEMOVE) {
            self->m_cursor_in_client = hit.in_client;
            if (is_sub) {
                self->subtract_brush_dab(uv_x, uv_y, rad_uv_x, rad_uv_y);
            } else if (self->m_current_stroke.empty() ||
                       std::hypot(self->m_current_stroke.back().x - uv_x, self->m_current_stroke.back().y - uv_y) > 0.001f) {
                self->m_current_stroke.push_back(ImVec2(uv_x, uv_y));
            }
            if (self->m_last_img_w > 0 && self->m_last_img_h > 0) {
                EraseTool &tool = EraseTool::get();
                tool.ensure_mask_size(self->m_last_img_w, self->m_last_img_h);
                tool.brush_radius = self->m_erase_radius;
                float px = uv_x * static_cast<float>(self->m_last_img_w);
                float py = uv_y * static_cast<float>(self->m_last_img_h);
                ImVec2 curr_px(px, py);
                if (self->m_last_paint_px.x >= 0.0f && self->m_last_paint_px.y >= 0.0f) {
                    tool.draw_brush_line(self->m_last_paint_px, curr_px, self->m_erase_radius, is_sub);
                } else {
                    tool.draw_brush_dab((int)std::round(px), (int)std::round(py), (int)std::round(self->m_erase_radius), is_sub ? 0 : 255);
                }
                self->m_last_paint_px = curr_px;
            }
            self->apply_cursor_for_state(hit);
            return true;
        }
        if (msg == WM_LBUTTONUP) {
            self->m_is_painting_mask = false;
            self->m_last_paint_px = ImVec2(-1.0f, -1.0f);
            if (!is_sub && !self->m_current_stroke.empty()) {
                self->m_erase_strokes.push_back(self->m_current_stroke);
                self->m_current_stroke.clear();
            }
            self->end_capture();
            self->apply_cursor_for_state(hit);
            self->on_erase_stroke_finished(is_sub);
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

    // splitter double-click and drag start
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

    bool space_active = self->m_space_held || ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0);
    self->vp_set_space_pan(space_active);

    // canvas input ownership when an image is loaded and no splitter/popup intercepts it
    if (hit.in_canvas && self->has_loaded_image() && !hit.popup_open) {
        if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) &&
            self->m_erase_active && self->m_active_tool == ActiveTool::Erase && !space_active) {
            if (ImGui::GetIO().WantTextInput) {
                ImGui::SetWindowFocus(nullptr);
            }
            float uv_x = 0.0f, uv_y = 0.0f, rad_uv_x = 0.01f, rad_uv_y = 0.01f;
            bool over_image = false;
            compute_brush_uv(uv_x, uv_y, rad_uv_x, rad_uv_y, over_image);
            if (over_image) {
                bool is_sub = self->m_erase_subtract || ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
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
                    float px = uv_x * static_cast<float>(self->m_last_img_w);
                    float py = uv_y * static_cast<float>(self->m_last_img_h);
                    self->m_last_paint_px = ImVec2(px, py);
                    tool.draw_brush_dab((int)std::round(px), (int)std::round(py), (int)std::round(self->m_erase_radius), is_sub ? 0 : 255);
                }
                self->apply_cursor_for_state(hit);
                return true;
            }
            return true;
        }

        if (msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_RBUTTONDOWN ||
            msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK ||
            msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
            if (ImGui::GetIO().WantTextInput) {
                ImGui::SetWindowFocus(nullptr);
            }
            bool is_down = (msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_RBUTTONDOWN);
            if (is_down) {
                // acquire win32 capture before viewport sets drag state so sync WM_CAPTURECHANGED cant cancel
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

    // UI panels, toolbar, menu bar, status bar, popups, empty canvas
    bool imgui_handled = ImGui_ImplWin32_WndProcHandler(self->m_hwnd, msg, wParam, lParam) != 0;
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
        auto ev = m_sim_mouse_queue.front();
        m_sim_mouse_queue.erase(m_sim_mouse_queue.begin());
        ImGuiIO &io = ImGui::GetIO();
        m_last_mouse_pt = { static_cast<LONG>(ev.x), static_cast<LONG>(ev.y) };
        m_cursor_in_client = true;
        io.AddMousePosEvent(ev.x, ev.y);
        if (ev.action == 1) { // Click
            io.AddMouseButtonEvent(0, true);
            m_sim_mouse_down_prev = true;
        } else if (ev.action == 0) { // Release / Move
            io.AddMouseButtonEvent(0, false);
        }
    }

    ImGui::NewFrame();
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
        ImGui::PushTextWrapPos(280.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "ShaderLab v1.3.0");
        ImGui::TextDisabled("Realtime Shader & Depth Compositing Studio");
        ImGui::Separator();
        ImGui::TextWrapped("High-performance D3D11 shader workbench for ReShade FX, Unity, and Godot shaders with neural inpainting and depth estimation.");
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
        ImGui::PushTextWrapPos(360.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "Keyboard Shortcuts Reference");
        ImGui::Separator();
        ImGui::TextUnformatted("General & Tools:");
        ImGui::BulletText("H: Hand / Pan Tool");
        ImGui::BulletText("E: Erase Brush Tool");
        ImGui::BulletText("D: 3D Depth Peek");
        ImGui::BulletText("B: Before / After Split Compare");
        ImGui::BulletText("\\: Master Bypass (Toggle Effects)");
        ImGui::BulletText("Tab: Show / Hide Side Panels");
        ImGui::Spacing();
        ImGui::TextUnformatted("View & Navigation:");
        ImGui::BulletText("Space + Drag: Hand Pan");
        ImGui::BulletText("Ctrl + + / =: Zoom In");
        ImGui::BulletText("Ctrl + -: Zoom Out");
        ImGui::BulletText("Ctrl + 0: Fit View to Screen");
        ImGui::BulletText("Ctrl + 1: Actual Size (100%)");
        ImGui::BulletText("Ctrl + 2: Zoom 200%");
        ImGui::BulletText("Home: Reset View (Pan & Zoom)");
        ImGui::Spacing();
        ImGui::TextUnformatted("Editing & Tabs:");
        ImGui::BulletText("Ctrl + O: Open Image");
        ImGui::BulletText("Ctrl + S: Save Snapshot");
        ImGui::BulletText("Ctrl + W: Close Active Tab");
        ImGui::BulletText("Ctrl + Tab: Cycle Tabs");
        ImGui::BulletText("[ / ]: Decrease / Increase Brush Radius");
        ImGui::BulletText("Alt (Hold): Subtract From Mask");
        ImGui::BulletText("Ctrl + Z: Undo Mask Stroke");
        ImGui::BulletText("Ctrl + Y / Ctrl+Shift+Z: Redo Mask Stroke");
        ImGui::BulletText("Esc: Clear Mask / Deselect Tool");
        ImGui::BulletText("F5: Reload Shaders");
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
    size_t close_pos = json.rfind('}');
    if (close_pos == std::string::npos) {
        return json;
    }
    size_t prev = close_pos;
    while (prev > 0 && (json[prev - 1] == ' ' || json[prev - 1] == '\t' || json[prev - 1] == '\r' || json[prev - 1] == '\n')) {
        --prev;
    }
    bool empty_obj = (prev > 0 && json[prev - 1] == '{');
    std::string field = empty_obj ? "" : ", ";
    field += "\"active_cursor\": \"" + m_active_cursor_name + "\", ";
    field += "\"mask_white_px\": " + std::to_string(EraseTool::get().get_mask_white_px()) + ", ";
    field += "\"erase_step\": " + std::to_string(EraseTool::get().get_current_erase_step()) + ", ";
    field += "\"erase_total\": " + std::to_string(EraseTool::get().get_total_erase_steps()) + ", ";
    field += "\"active_tab\": " + std::to_string(m_active_tab) + ", ";
    field += "\"tab_count\": " + std::to_string(static_cast<int>(m_tabs.size()));
    json.insert(close_pos, field);
    return json;
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

        EraseTool &tool = EraseTool::get();
        tool.ensure_mask_size(m_last_img_w, m_last_img_h);
        tool.brush_radius = m_erase_radius;
        ImVec2 curr_px(clamped_px_x, clamped_px_y);

        bool finished_non_sub_stroke = false;
        if (act == 2) { // down
            m_is_painting_mask = true;
            push_erase_undo();
            tool.push_undo();
            m_last_paint_px = curr_px;
            tool.draw_brush_dab((int)std::round(curr_px.x), (int)std::round(curr_px.y), (int)std::round(m_erase_radius), is_sub ? 0 : 255);
            if (is_sub) {
                subtract_brush_dab(uv_x, uv_y, rad_uv_x, rad_uv_y);
            } else {
                m_current_stroke.clear();
                m_current_stroke.push_back(ImVec2(uv_x, uv_y));
            }
        } else if (act == 1) { // single click (down + up)
            m_is_painting_mask = false;
            m_last_paint_px = ImVec2(-1.0f, -1.0f);
            push_erase_undo();
            tool.push_undo();
            tool.draw_brush_dab((int)std::round(curr_px.x), (int)std::round(curr_px.y), (int)std::round(m_erase_radius), is_sub ? 0 : 255);
            if (is_sub) {
                subtract_brush_dab(uv_x, uv_y, rad_uv_x, rad_uv_y);
            } else {
                m_erase_strokes.push_back({ ImVec2(uv_x, uv_y) });
                finished_non_sub_stroke = true;
            }
        } else if (act == 0 && m_is_painting_mask) { // move
            if (m_last_paint_px.x >= 0.0f && m_last_paint_px.y >= 0.0f) {
                tool.draw_brush_line(m_last_paint_px, curr_px, m_erase_radius, is_sub);
            } else {
                tool.draw_brush_dab((int)std::round(curr_px.x), (int)std::round(curr_px.y), (int)std::round(m_erase_radius), is_sub ? 0 : 255);
            }
            m_last_paint_px = curr_px;
            if (is_sub) {
                subtract_brush_dab(uv_x, uv_y, rad_uv_x, rad_uv_y);
            } else {
                m_current_stroke.push_back(ImVec2(uv_x, uv_y));
            }
        } else if (act == 3 && m_is_painting_mask) { // up
            m_is_painting_mask = false;
            m_last_paint_px = ImVec2(-1.0f, -1.0f);
            if (!is_sub && !m_current_stroke.empty()) {
                m_erase_strokes.push_back(m_current_stroke);
                m_current_stroke.clear();
            }
            if (!is_sub) finished_non_sub_stroke = true;
        }
        if (m_device) {
            tool.update_gpu_texture(m_device);
        }
        if (finished_non_sub_stroke) {
            on_erase_stroke_finished(false);
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
    if (id_or_label == "erase_checkbox_auto_apply") { EraseTool::get().auto_apply = !EraseTool::get().auto_apply; return true; }
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
