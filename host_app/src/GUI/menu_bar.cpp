#include "menu_bar.h"
#include "gui_manager.h"
#include "theme.h"
#include "../erase_tool.h"
#include <imgui.h>

namespace gui {

void render_menu_bar(GuiManager &gui) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Image...", "Ctrl+O")) {
                std::wstring path;
                if (open_file_dialog(gui.m_hwnd, L"Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files (*.*)\0*.*\0", path)) {
                    gui.m_req_open_image = true;
                    gui.m_open_image_path = path;
                }
            }
            if (ImGui::MenuItem("Save Snapshot...", "Ctrl+S")) {
                std::wstring path;
                if (save_file_dialog(gui.m_hwnd, L"PNG Image (*.png)\0*.png\0JPEG Image (*.jpg)\0*.jpg\0All Files (*.*)\0*.*\0", L"png", path)) {
                    gui.m_req_save_snapshot = true;
                    gui.m_save_snapshot_path = path;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Close Tab", "Ctrl+W", false, !gui.m_tabs.empty())) {
                gui.m_req_close_tab = gui.m_active_tab;
            }
            if (ImGui::MenuItem("Close All Tabs", nullptr, false, !gui.m_tabs.empty())) {
                gui.m_req_close_all_tabs = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Attach .sldepth File...")) {
                std::wstring path;
                if (open_file_dialog(gui.m_hwnd, L"ShaderLab Depth (*.sldepth)\0*.sldepth\0All Files (*.*)\0*.*\0", path)) {
                    gui.m_req_attach_depth = true;
                    gui.m_attach_depth_path = path;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                PostMessageW(gui.m_hwnd, WM_CLOSE, 0, 0);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            bool can_undo_erase = (gui.m_active_tool == ActiveTool::Erase && !gui.m_erase_undo_stack.empty());
            if (ImGui::MenuItem("Undo Mask Stroke", "Ctrl+Z", false, can_undo_erase)) {
                gui.undo_erase_stroke();
            }
            bool can_redo_erase = (gui.m_active_tool == ActiveTool::Erase && !gui.m_erase_redo_stack.empty());
            if (ImGui::MenuItem("Redo Mask Stroke", "Ctrl+Y", false, can_redo_erase)) {
                gui.redo_erase_stroke();
            }
            ImGui::Separator();
            bool has_mask = !gui.m_erase_strokes.empty() || EraseTool::get().has_mask_edits();
            if (ImGui::MenuItem("Clear Erase Mask", "Esc", false, has_mask)) {
                gui.clear_erase_strokes();
                gui.m_req_clear_mask = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reset View", "Home")) {
                gui.m_req_reset_view = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Static Zoom (Center-anchored)", nullptr, &gui.m_static_zoom)) {}
            gui.register_dom_last_item("menu_opt_static_zoom", "Static Zoom (Center-anchored)", "menu_item", gui.m_static_zoom);
            gui.register_dom_last_item("opt_static_zoom", "Static Zoom", "menu_item", gui.m_static_zoom);

            ImGui::Separator();
            if (ImGui::MenuItem("Invert Depth Buffer", nullptr, &gui.m_depth_invert)) {}
            ImGui::Separator();
            if (ImGui::MenuItem("Hide Side Panels", "Tab", &gui.m_hide_panels)) {}
            ImGui::EndMenu();
        } else {
            gui.m_dom.register_item("opt_static_zoom", "Static Zoom", "setting", ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f), gui.m_static_zoom, true);
        }

        if (ImGui::BeginMenu("Shaders")) {
            if (ImGui::MenuItem("Add ReShade Shader (.fx)...")) {
                std::wstring path;
                if (open_file_dialog(gui.m_hwnd, L"ReShade FX Files (*.fx)\0*.fx\0All Files (*.*)\0*.*\0", path)) {
                    gui.m_req_add_shader = true;
                    gui.m_add_shader_path = path;
                }
            }
            if (ImGui::MenuItem("Import Unity Shader (.shader)...")) {
                std::wstring path;
                if (open_file_dialog(gui.m_hwnd, L"Unity Shaders (*.shader)\0*.shader\0All Files (*.*)\0*.*\0", path)) {
                    gui.m_req_add_shader = true;
                    gui.m_add_shader_path = path;
                }
            }
            if (ImGui::MenuItem("Import Godot Shader (.gdshader)...")) {
                std::wstring path;
                if (open_file_dialog(gui.m_hwnd, L"Godot Shaders (*.gdshader)\0*.gdshader\0All Files (*.*)\0*.*\0", path)) {
                    gui.m_req_add_shader = true;
                    gui.m_add_shader_path = path;
                }
            }
            if (ImGui::MenuItem("Import Raw HLSL (.hlsl)...")) {
                std::wstring path;
                if (open_file_dialog(gui.m_hwnd, L"HLSL Files (*.hlsl)\0*.hlsl\0All Files (*.*)\0*.*\0", path)) {
                    gui.m_req_add_shader = true;
                    gui.m_add_shader_path = path;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Enable All Shaders")) {
                for (auto &fx : gui.m_effects) {
                    fx.enabled = true;
                }
            }
            if (ImGui::MenuItem("Disable All Shaders")) {
                for (auto &fx : gui.m_effects) {
                    fx.enabled = false;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reload All Shaders", "F5")) {
                gui.m_effects.clear();
                add_sample_effects(gui.m_effects);
                gui.m_selected_effect_idx = 0;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Zoom In", "Ctrl++ / Ctrl+=")) {
                gui.m_req_zoom_in = true;
            }
            if (ImGui::MenuItem("Zoom Out", "Ctrl+-")) {
                gui.m_req_zoom_out = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Fit Canvas to Screen", "Ctrl+0")) {
                gui.m_req_fit_view = true;
            }
            if (ImGui::MenuItem("Actual Size (100%)", "Ctrl+1")) {
                gui.m_req_100_view = true;
            }
            if (ImGui::MenuItem("Zoom 200%", "Ctrl+2")) {
                gui.m_req_200_view = true;
            }
            ImGui::Separator();
            bool split_active = (gui.m_active_tool == ActiveTool::Split);
            if (ImGui::MenuItem("Before / After Split (B)", nullptr, &split_active)) {
                gui.set_active_tool(split_active ? ActiveTool::Split : ActiveTool::Hand);
            }
            bool depth_active = (gui.m_active_tool == ActiveTool::Depth);
            if (ImGui::MenuItem("3D Depth Calibration", "Hold D to peek", &depth_active)) {
                gui.set_active_tool(depth_active ? ActiveTool::Depth : ActiveTool::Hand);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Toggle Side Panels", "Tab", &gui.m_hide_panels)) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Keyboard Shortcuts", "F1")) {
                gui.m_show_shortcuts_dialog = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("About ShaderLab")) {
                gui.m_show_about_dialog = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

} // namespace gui
