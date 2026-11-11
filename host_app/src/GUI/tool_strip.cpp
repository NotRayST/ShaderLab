#include "tool_strip.h"
#include "gui_manager.h"
#include <imgui.h>
#include <cmath>

namespace gui {

void render_tool_strip(GuiManager &gui, float top_offset, float height) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, top_offset));
    ImGui::SetNextWindowSize(ImVec2(gui.get_toolbar_width(), height));
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
        gui.register_dom_last_item(dom_id, tooltip, "tool_button", is_active);
        if (strcmp(label, "H") == 0) gui.register_dom_last_item("tool_hand", tooltip, "tool_button", is_active);
        else if (strcmp(label, "E") == 0) gui.register_dom_last_item("tool_erase", tooltip, "tool_button", is_active);
        else if (strcmp(label, "D") == 0) gui.register_dom_last_item("tool_depth", tooltip, "tool_button", is_active);
        else if (strcmp(label, "B") == 0) gui.register_dom_last_item("tool_split", tooltip, "tool_button", is_active);
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

    render_tool_btn("H", "Hand Tool (H)\nPan and zoom the canvas", gui.m_active_tool == ActiveTool::Hand, ToolIconType::Hand, [&]() {
        gui.set_active_tool(ActiveTool::Hand);
    });

    render_tool_btn("E", "Erase Brush (E)\nPaint mask to remove objects with AI inpainting", gui.m_active_tool == ActiveTool::Erase, ToolIconType::Erase, [&]() {
        if (gui.m_active_tool == ActiveTool::Erase) {
            gui.set_active_tool(ActiveTool::Hand);
        } else {
            gui.set_active_tool(ActiveTool::Erase);
        }
    });

    render_tool_btn("D", "3D Depth (D)\nPeek or configure depth buffer", gui.m_active_tool == ActiveTool::Depth, ToolIconType::Depth, [&]() {
        if (gui.m_active_tool == ActiveTool::Depth) {
            gui.set_active_tool(ActiveTool::Hand);
        } else {
            gui.set_active_tool(ActiveTool::Depth);
        }
    });

    render_tool_btn("B", "Split Compare (B)\nBefore / After interactive split", gui.m_active_tool == ActiveTool::Split, ToolIconType::Split, [&]() {
        if (gui.m_active_tool == ActiveTool::Split) {
            gui.set_active_tool(ActiveTool::Hand);
        } else {
            gui.set_active_tool(ActiveTool::Split);
        }
    });

    ImGui::Separator();
    ImGui::Spacing();

    render_tool_btn("\\", "Master Bypass (\\)\nToggle all shaders to view raw original", gui.m_master_bypass, ToolIconType::Bypass, [&]() {
        gui.m_master_bypass = !gui.m_master_bypass;
    });

    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(gui.get_toolbar_width() - 0.5f, top_offset),
                  ImVec2(gui.get_toolbar_width() - 0.5f, top_offset + height),
                  IM_COL32(36, 36, 40, 220), 1.0f);

    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace gui
