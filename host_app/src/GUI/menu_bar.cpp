#include "menu_bar.h"
#include "gui_manager.h"
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
            if (ImGui::MenuItem("Reset View", "Home")) {
                gui.m_req_reset_view = true;
            }
            if (ImGui::MenuItem("Clear Erase Mask", "Esc")) {
                gui.m_req_clear_mask = true;
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
                gui.m_req_fit_view = true;
            }
            if (ImGui::MenuItem("Actual Size (100%)", "Ctrl+1")) {
                gui.m_req_100_view = true;
            }
            ImGui::Separator();
            bool split_active = (gui.m_active_tool == ActiveTool::Split);
            if (ImGui::MenuItem("Before / After Split (B)", nullptr, &split_active)) {
                gui.set_active_tool(split_active ? ActiveTool::Split : ActiveTool::Hand);
            }
            bool depth_active = (gui.m_active_tool == ActiveTool::Depth);
            if (ImGui::MenuItem("Peek 3D Depth (D)", nullptr, &depth_active)) {
                gui.set_active_tool(depth_active ? ActiveTool::Depth : ActiveTool::Hand);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About ShaderLab")) {
                gui.m_show_about_dialog = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

} // namespace gui
