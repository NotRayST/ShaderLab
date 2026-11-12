#include "left_panel.h"
#include "gui_manager.h"
#include <imgui.h>
#include <algorithm>

namespace gui {

void render_left_panel(GuiManager &gui, float left_offset, float top_offset, float panel_width, float panel_height) {
    ImGui::SetNextWindowPos(ImVec2(left_offset, top_offset));
    ImGui::SetNextWindowSize(ImVec2(panel_width, panel_height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
    ImGui::Begin("##LeftShadersPanel", nullptr, flags);

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("SHADERS & EFFECTS");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##Filter", "Filter shaders...", gui.m_search_buffer, sizeof(gui.m_search_buffer));
    gui.register_dom_last_item("filter_shaders", "Filter shaders", "input");
    ImGui::Spacing();

    float btn_h = 24.0f;
    float spacing_y = ImGui::GetStyle().ItemSpacing.y;
    float bottom_controls_h = btn_h * 2.0f + spacing_y;
    ImGui::BeginChild("##EffectListRegion", ImVec2(-1.0f, -(bottom_controls_h + spacing_y)), true);

    std::string search_query = gui.m_search_buffer;
    std::transform(search_query.begin(), search_query.end(), search_query.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });

    for (size_t i = 0; i < gui.m_effects.size(); ++i) {
        auto &fx = gui.m_effects[i];

        if (!search_query.empty()) {
            std::string lower_name = fx.name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
            if (lower_name.find(search_query) == std::string::npos) continue;
        }

        ImGui::PushID(static_cast<int>(i));

        bool is_selected = (gui.m_selected_effect_idx == static_cast<int>(i));
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
        gui.register_dom_last_item("shader_checkbox_" + std::to_string(i), fx.name + " Checkbox", "checkbox", fx.enabled);

        // clickable label region for selecting this effect
        ImGui::SetCursorScreenPos(ImVec2(row_x + 30.0f, p0.y + 2.0f));
        float select_w = (std::max)(20.0f, row_w - 34.0f);
        if (ImGui::InvisibleButton("##select_area", ImVec2(select_w, row_h - 4.0f))) {
            gui.m_selected_effect_idx = static_cast<int>(i);
        }
        gui.register_dom_last_item("shader_select_" + std::to_string(i), fx.name, "list_item", is_selected);
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
        if (open_file_dialog(gui.m_hwnd, L"Shader Files (*.fx;*.shader;*.gdshader;*.hlsl)\0*.fx;*.shader;*.gdshader;*.hlsl\0All Files (*.*)\0*.*\0", path)) {
            gui.m_req_add_shader = true;
            gui.m_add_shader_path = path;
        }
    }
    gui.register_dom_last_item("btn_add_shader", "+ Add Shader", "button");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open and load a shader effect into pipeline");

    float btn_w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    bool can_move_up = (gui.m_selected_effect_idx > 0 && gui.m_selected_effect_idx < static_cast<int>(gui.m_effects.size()));
    if (!can_move_up) ImGui::BeginDisabled();
    if (ImGui::Button("Move Up", ImVec2(btn_w, btn_h))) {
        if (can_move_up) {
            std::swap(gui.m_effects[gui.m_selected_effect_idx], gui.m_effects[gui.m_selected_effect_idx - 1]);
            gui.m_selected_effect_idx--;
        }
    }
    gui.register_dom_last_item("btn_move_up", "Move Up", "button", false, can_move_up);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move selected shader earlier in execution order");
    if (!can_move_up) ImGui::EndDisabled();

    ImGui::SameLine();

    bool can_move_down = (gui.m_selected_effect_idx >= 0 && gui.m_selected_effect_idx + 1 < static_cast<int>(gui.m_effects.size()));
    if (!can_move_down) ImGui::BeginDisabled();
    if (ImGui::Button("Move Down", ImVec2(btn_w, btn_h))) {
        if (can_move_down) {
            std::swap(gui.m_effects[gui.m_selected_effect_idx], gui.m_effects[gui.m_selected_effect_idx + 1]);
            gui.m_selected_effect_idx++;
        }
    }
    gui.register_dom_last_item("btn_move_down", "Move Down", "button", false, can_move_down);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move selected shader later in execution order");
    if (!can_move_down) ImGui::EndDisabled();

    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(left_offset + panel_width - 0.5f, top_offset),
                  ImVec2(left_offset + panel_width - 0.5f, top_offset + panel_height),
                  gui.m_resizing_left ? IM_COL32(75, 140, 220, 240) : IM_COL32(36, 36, 40, 220), gui.m_resizing_left ? 2.0f : 1.0f);

    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace gui
