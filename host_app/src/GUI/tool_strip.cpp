#include "tool_strip.h"
#include "gui_manager.h"

namespace gui {

namespace {

void draw_tool_icon(ImDrawList *draw, ToolIconType icon, ImVec2 center, ImU32 col) {
    const float cx = std::floor(center.x) + 0.5f;
    const float cy = std::floor(center.y) + 0.5f;
    constexpr float kStroke = 1.5f;

    switch (icon) {
    case ToolIconType::Move: {
        draw->AddLine(ImVec2(cx - 6.5f, cy), ImVec2(cx + 6.5f, cy), col, kStroke);
        draw->AddLine(ImVec2(cx, cy - 6.5f), ImVec2(cx, cy + 6.5f), col, kStroke);
        draw->AddTriangleFilled(ImVec2(cx, cy - 8.5f), ImVec2(cx - 2.6f, cy - 5.0f), ImVec2(cx + 2.6f, cy - 5.0f), col);
        draw->AddTriangleFilled(ImVec2(cx, cy + 8.5f), ImVec2(cx - 2.6f, cy + 5.0f), ImVec2(cx + 2.6f, cy + 5.0f), col);
        draw->AddTriangleFilled(ImVec2(cx - 8.5f, cy), ImVec2(cx - 5.0f, cy - 2.6f), ImVec2(cx - 5.0f, cy + 2.6f), col);
        draw->AddTriangleFilled(ImVec2(cx + 8.5f, cy), ImVec2(cx + 5.0f, cy - 2.6f), ImVec2(cx + 5.0f, cy + 2.6f), col);
        break;
    }
    case ToolIconType::Hand: {
        // Palm base + 4 vertical fingers + angled thumb
        draw->AddRect(ImVec2(cx - 4.0f, cy - 0.5f), ImVec2(cx + 4.5f, cy + 7.0f), col, 2.5f, 0, kStroke);
        draw->AddLine(ImVec2(cx - 3.5f, cy), ImVec2(cx - 3.5f, cy - 5.5f), col, kStroke);
        draw->AddLine(ImVec2(cx - 1.0f, cy - 0.5f), ImVec2(cx - 1.0f, cy - 7.5f), col, kStroke);
        draw->AddLine(ImVec2(cx + 1.5f, cy - 0.5f), ImVec2(cx + 1.5f, cy - 7.0f), col, kStroke);
        draw->AddLine(ImVec2(cx + 4.0f, cy), ImVec2(cx + 4.0f, cy - 4.5f), col, kStroke);
        draw->AddLine(ImVec2(cx - 4.0f, cy + 3.5f), ImVec2(cx - 7.5f, cy + 0.5f), col, kStroke);
        break;
    }
    case ToolIconType::Rotate: {
        constexpr float kPi = 3.14159265f;
        draw->PathClear();
        draw->PathArcTo(ImVec2(cx, cy), 6.5f, -kPi * 0.75f, kPi * 0.85f, 18);
        draw->PathStroke(col, ImDrawFlags_None, kStroke);
        // Arrowhead at -0.75 * pi (upper-left end of arc)
        const float ax = cx + std::cos(-kPi * 0.75f) * 6.5f;
        const float ay = cy + std::sin(-kPi * 0.75f) * 6.5f;
        draw->AddTriangleFilled(ImVec2(ax - 1.5f, ay + 2.0f), ImVec2(ax - 2.5f, ay - 3.5f), ImVec2(ax + 3.0f, ay - 2.0f), col);
        // Small level tick through center
        draw->AddLine(ImVec2(cx - 3.5f, cy), ImVec2(cx + 3.5f, cy), col, 1.2f);
        break;
    }
    case ToolIconType::Crop: {
        const ImVec2 left_bracket[] = {
            ImVec2(cx - 5.0f, cy - 8.0f),
            ImVec2(cx - 5.0f, cy + 5.0f),
            ImVec2(cx + 8.0f, cy + 5.0f)
        };
        draw->AddPolyline(left_bracket, 3, col, ImDrawFlags_None, kStroke);
        const ImVec2 right_bracket[] = {
            ImVec2(cx - 8.0f, cy - 5.0f),
            ImVec2(cx + 5.0f, cy - 5.0f),
            ImVec2(cx + 5.0f, cy + 8.0f)
        };
        draw->AddPolyline(right_bracket, 3, col, ImDrawFlags_None, kStroke);
        break;
    }
    case ToolIconType::RasterErase: {
        // Tilted rectangular eraser block with sleeve divider line + baseline
        const ImVec2 p0(cx - 1.0f, cy - 7.0f);
        const ImVec2 p1(cx + 7.0f, cy - 1.0f);
        const ImVec2 p2(cx + 0.5f, cy + 6.5f);
        const ImVec2 p3(cx - 7.0f, cy + 0.5f);
        draw->AddQuad(p0, p1, p2, p3, col, kStroke);
        draw->AddLine(ImVec2(cx - 3.8f, cy - 3.0f), ImVec2(cx + 3.6f, cy + 3.0f), col, kStroke);
        draw->AddLine(ImVec2(cx - 6.0f, cy + 7.5f), ImVec2(cx + 7.5f, cy + 7.5f), col, kStroke);
        break;
    }
    case ToolIconType::AiInpaint: {
        // Healing brush circle + top-right 4-point AI spark
        draw->AddCircle(ImVec2(cx - 1.5f, cy + 1.5f), 5.0f, col, 16, kStroke);
        draw->AddLine(ImVec2(cx - 5.0f, cy + 5.0f), ImVec2(cx + 2.0f, cy - 2.0f), col, kStroke);
        // 4-point AI star at upper right
        const float sx = cx + 5.5f;
        const float sy = cy - 5.5f;
        draw->AddLine(ImVec2(sx - 3.0f, sy), ImVec2(sx + 3.0f, sy), col, 1.3f);
        draw->AddLine(ImVec2(sx, sy - 3.0f), ImVec2(sx, sy + 3.0f), col, 1.3f);
        break;
    }
    case ToolIconType::Text: {
        // Bold crisp uppercase 'T' with tiny serif ticks
        draw->AddLine(ImVec2(cx - 6.5f, cy - 6.5f), ImVec2(cx + 6.5f, cy - 6.5f), col, 1.8f);
        draw->AddLine(ImVec2(cx - 6.5f, cy - 6.5f), ImVec2(cx - 6.5f, cy - 4.0f), col, kStroke);
        draw->AddLine(ImVec2(cx + 6.5f, cy - 6.5f), ImVec2(cx + 6.5f, cy - 4.0f), col, kStroke);
        draw->AddLine(ImVec2(cx, cy - 6.5f), ImVec2(cx, cy + 7.0f), col, 1.8f);
        draw->AddLine(ImVec2(cx - 3.5f, cy + 7.0f), ImVec2(cx + 3.5f, cy + 7.0f), col, kStroke);
        break;
    }
    case ToolIconType::Upscale: {
        // Source box + outer frame + diagonal expand arrow
        draw->AddRect(ImVec2(cx - 7.0f, cy + 1.0f), ImVec2(cx - 1.0f, cy + 7.0f), col, 1.0f, 0, kStroke);
        draw->AddRect(ImVec2(cx - 7.0f, cy - 7.0f), ImVec2(cx + 7.0f, cy + 7.0f), col, 1.5f, 0, kStroke);
        draw->AddLine(ImVec2(cx - 1.0f, cy + 1.0f), ImVec2(cx + 4.5f, cy - 4.5f), col, kStroke);
        draw->AddTriangleFilled(ImVec2(cx + 5.5f, cy - 5.5f), ImVec2(cx + 1.0f, cy - 5.0f), ImVec2(cx + 5.0f, cy - 1.0f), col);
        break;
    }
    case ToolIconType::Depth: {
        // Isometric 3D wireframe cube
        const ImVec2 top(cx, cy - 7.5f);
        const ImVec2 mid(cx, cy + 0.5f);
        const ImVec2 r_top(cx + 7.0f, cy - 3.5f);
        const ImVec2 r_bot(cx + 7.0f, cy + 4.0f);
        const ImVec2 bot(cx, cy + 8.0f);
        const ImVec2 l_bot(cx - 7.0f, cy + 4.0f);
        const ImVec2 l_top(cx - 7.0f, cy - 3.5f);
        const ImVec2 hex[] = { top, r_top, r_bot, bot, l_bot, l_top };
        draw->AddPolyline(hex, 6, col, ImDrawFlags_Closed, kStroke);
        draw->AddLine(l_top, mid, col, kStroke);
        draw->AddLine(r_top, mid, col, kStroke);
        draw->AddLine(mid, bot, col, kStroke);
        break;
    }
    case ToolIconType::Split: {
        draw->AddRect(ImVec2(cx - 7.0f, cy - 6.0f), ImVec2(cx + 7.0f, cy + 6.0f), col, 1.5f, 0, kStroke);
        draw->AddRectFilled(ImVec2(cx - 7.0f, cy - 6.0f), ImVec2(cx, cy + 6.0f), (col & 0x00FFFFFF) | 0x44000000);
        draw->AddLine(ImVec2(cx, cy - 8.0f), ImVec2(cx, cy + 8.0f), col, kStroke);
        break;
    }
    case ToolIconType::Bypass: {
        constexpr float kPi = 3.14159265f;
        draw->PathClear();
        draw->PathArcTo(ImVec2(cx, cy + 1.0f), 6.0f, -kPi * 0.25f, kPi * 1.25f, 16);
        draw->PathStroke(col, ImDrawFlags_None, kStroke);
        draw->AddLine(ImVec2(cx, cy - 7.5f), ImVec2(cx, cy + 0.5f), col, kStroke);
        break;
    }
    default:
        break;
    }
}

void render_tool_button(GuiManager &gui,
                        const char *dom_id,
                        const char *imgui_id,
                        const char *label,
                        const char *tooltip,
                        ToolIconType icon,
                        bool active,
                        ImU32 active_bar_col,
                        const std::function<void()> &on_click) {
    constexpr ImVec2 kBtnSize(28.0f, 27.0f);

    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.45f, 0.72f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.52f, 0.80f, 1.00f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.20f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.27f, 0.30f, 0.85f));
    }

    if (ImGui::Button(imgui_id, kBtnSize)) {
        on_click();
    }
    gui.register_dom_last_item(dom_id, label, "button", active, true);

    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    const ImVec2 center((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImU32 icon_col = active ? IM_COL32(255, 255, 255, 255) : IM_COL32(205, 208, 214, 240);
    draw_tool_icon(draw, icon, center, icon_col);

    if (active) {
        draw->AddRectFilled(ImVec2(p1.x + 1.0f, p0.y + 4.0f), ImVec2(p1.x + 3.0f, p1.y - 4.0f), active_bar_col, 1.0f);
    }

    ImGui::PopStyleColor(2);

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", tooltip);
    }
}

} // namespace

void render_tool_strip(GuiManager &gui, float top_offset, float height) {
    const float w = gui.get_toolbar_width();
    ImGui::SetNextWindowPos(ImVec2(0.0f, top_offset));
    ImGui::SetNextWindowSize(ImVec2(w, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;

    if (ImGui::Begin("##LeftToolStrip", nullptr, kFlags)) {
        constexpr ImU32 kBlueBar = IM_COL32(95, 175, 255, 255);
        constexpr ImU32 kAmberBar = IM_COL32(245, 165, 65, 255);
        constexpr ImU32 kCyanBar = IM_COL32(75, 205, 215, 255);
        constexpr ImU32 kRedBar = IM_COL32(235, 95, 95, 255);

        render_tool_button(
            gui, "tool_V", "##tool_v", "Move Tool (V)",
            "Move Tool (V)\nMove active layer inside its document frame\nHold Shift: constrain axis | Arrows: nudge",
            ToolIconType::Move, gui.m_active_tool == ActiveTool::Move, kCyanBar,
            [&]() { gui.set_active_tool(ActiveTool::Move); });

        render_tool_button(
            gui, "tool_H", "##tool_h", "Hand Tool (H)",
            "Hand Tool (H)\nDrag canvas to pan viewport, mouse wheel to zoom",
            ToolIconType::Hand, gui.m_active_tool == ActiveTool::Hand, kBlueBar,
            [&]() { gui.set_active_tool(ActiveTool::Hand); });

        render_tool_button(
            gui, "tool_R", "##tool_r", "Rotate & Horizon Align (R)",
            "Rotate & Horizon Align (R)\nDrag a line along any tilted horizon or edge to level",
            ToolIconType::Rotate, gui.m_active_tool == ActiveTool::Rotate, kBlueBar,
            [&]() { gui.set_active_tool(ActiveTool::Rotate); });

        render_tool_button(
            gui, "tool_C", "##tool_c", "Crop Tool (C)",
            "Crop Tool (C)\nCrop document canvas using aspect ratio presets & Rule of Thirds",
            ToolIconType::Crop, gui.m_active_tool == ActiveTool::Crop, kBlueBar,
            [&]() { gui.set_active_tool(ActiveTool::Crop); });

        render_tool_button(
            gui, "tool_E", "##tool_e", "Eraser Tool (E)",
            "Eraser Tool (E)\nErase pixels to transparency (Size, Hardness, Opacity)",
            ToolIconType::RasterErase, gui.m_active_tool == ActiveTool::RasterErase, kBlueBar,
            [&]() { gui.set_active_tool(ActiveTool::RasterErase); });

        render_tool_button(
            gui, "tool_J", "##tool_j", "AI Object Erase (J)",
            "AI Object Erase (J)\nRemove objects with LaMa Big HD neural inpainting",
            ToolIconType::AiInpaint, gui.m_active_tool == ActiveTool::AiInpaint, kBlueBar,
            [&]() {
                gui.set_active_tool(
                    gui.m_active_tool == ActiveTool::AiInpaint ? ActiveTool::Hand : ActiveTool::AiInpaint);
            });

        render_tool_button(
            gui, "tool_T", "##tool_t", "Text Tool (T)",
            "Text Tool (T)\nAdd and style system-font DirectWrite text layers",
            ToolIconType::Text, gui.m_active_tool == ActiveTool::Text, kBlueBar,
            [&]() { gui.set_active_tool(ActiveTool::Text); });

        render_tool_button(
            gui, "tool_U", "##tool_u", "AI Upscaler (U)",
            "AI Upscaler (U)\nEnhance image resolution 2x / 4x (Real-ESRGAN / RealPLKSR)",
            ToolIconType::Upscale, gui.m_active_tool == ActiveTool::Upscale, kBlueBar,
            [&]() { gui.set_active_tool(ActiveTool::Upscale); });

        render_tool_button(
            gui, "tool_D", "##tool_d", "3D Depth (D)",
            "3D Depth Calibration (D)\nHold D anytime to temporarily peek depth",
            ToolIconType::Depth, gui.m_active_tool == ActiveTool::Depth, kBlueBar,
            [&]() {
                gui.set_active_tool(
                    gui.m_active_tool == ActiveTool::Depth ? ActiveTool::Hand : ActiveTool::Depth);
            });

        render_tool_button(
            gui, "tool_B", "##tool_b", "Split Compare (B)",
            "Split Before/After (B)\nCompare original image against shader/layer result",
            ToolIconType::Split, gui.m_active_tool == ActiveTool::Split, kAmberBar,
            [&]() {
                gui.set_active_tool(
                    gui.m_active_tool == ActiveTool::Split ? ActiveTool::Hand : ActiveTool::Split);
            });

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        render_tool_button(
            gui, "tool_\\", "##tool_bypass", "Master Bypass (\\)",
            "Master Bypass (\\)\nBypass all active shader effects",
            ToolIconType::Bypass, gui.m_master_bypass, kRedBar,
            [&]() { gui.m_master_bypass = !gui.m_master_bypass; });
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

} // namespace gui
