#include "status_bar.h"
#include "gui_manager.h"
#include <imgui.h>
#include <algorithm>

namespace gui {

void render_bottom_status_bar(GuiManager &gui, float top_offset, float width, float height,
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
        float canvas_w = (std::max)(64.0f, width - gui.get_toolbar_width() - gui.get_left_panel_width(static_cast<uint32_t>(width)) - gui.get_right_panel_width(static_cast<uint32_t>(width)));
        float canvas_h = (std::max)(64.0f, gui.m_canvas_view_h > 0.0f ? gui.m_canvas_view_h : 600.0f);
        float fit_scale = (img_w > 0 && img_h > 0) ? (std::min)(canvas_w / static_cast<float>(img_w), canvas_h / static_cast<float>(img_h)) : 1.0f;
        float true_percent = zoom_level * fit_scale * 100.0f;
        ImGui::Text("Canvas: %ux%u | Zoom: %.0f%% | Pan: (%.0f, %.0f)",
                    img_w, img_h, true_percent, pan_x, pan_y);

        if (gui.m_master_bypass) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "[Bypass: Raw Original]");
        }
        if (gui.m_active_tool == ActiveTool::Depth && gui.m_depth_peek) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[Depth Peek]");
        }
        if (gui.m_active_tool == ActiveTool::Split && gui.m_before_after) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "[Split]");
        }
        if (gui.m_active_tool == ActiveTool::Erase && gui.m_erase_active) {
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
            gui.m_req_fit_view = true;
        }
        gui.register_dom_last_item("status_btn_fit", "Fit", "button");
        gui.register_dom_last_item("opt_fit", "Fit", "button");
        ImGui::SameLine();
        if (ImGui::SmallButton("100%")) {
            gui.m_req_100_view = true;
        }
        gui.register_dom_last_item("status_btn_100", "100%", "button");
        gui.register_dom_last_item("opt_100", "100%", "button");
        ImGui::SameLine();
        if (ImGui::SmallButton("200%")) {
            gui.m_req_200_view = true;
        }
        gui.register_dom_last_item("status_btn_200", "200%", "button");
        gui.register_dom_last_item("opt_200", "200%", "button");
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

} // namespace gui
