#include "status_bar.h"
#include "gui_manager.h"

#include <cstdio>

namespace gui {

void render_bottom_status_bar(GuiManager &gui, float top_offset, float width, float height,
                              uint32_t img_w, uint32_t img_h,
                              float zoom_level, float pan_x, float pan_y) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, top_offset));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;

    if (ImGui::Begin("##BottomStatusBar", nullptr, kFlags)) {
        ImGui::AlignTextToFramePadding();

        const auto &layer_view = gui.get_active_layer_view();
        if (img_w > 0 && img_h > 0) {
            if (layer_view.valid) {
                ImGui::Text("Canvas: %ux%u | Layer: %s (%s %d%%) | Zoom: %.0f%% | Pan: (%.0f, %.0f)",
                            img_w, img_h,
                            layer_view.layer_name.c_str(),
                            layer_view.blend_mode_name.c_str(),
                            layer_view.opacity_pct,
                            zoom_level * 100.0f, pan_x, pan_y);
            } else {
                ImGui::Text("Canvas: %ux%u | Zoom: %.0f%% | Pan: (%.0f, %.0f)",
                            img_w, img_h, zoom_level * 100.0f, pan_x, pan_y);
            }
        } else {
            ImGui::TextDisabled("No Image Loaded");
        }

        auto badge = [](ImVec4 color, const char *fmt, ...) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            va_list args;
            va_start(args, fmt);
            char buf[192] = {};
            vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);
            ImGui::TextColored(color, "%s", buf);
        };

        if (gui.m_master_bypass) {
            badge(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "BYPASS ACTIVE");
        }
        if (gui.is_depth_peek_active()) {
            badge(ImVec4(0.45f, 0.85f, 1.00f, 1.0f), "3D DEPTH PEEK");
        }
        if (gui.m_before_after) {
            badge(ImVec4(0.95f, 0.72f, 0.35f, 1.0f), "SPLIT COMPARE");
        }

        switch (gui.m_active_tool) {
        case ActiveTool::Move:
            badge(ImVec4(0.40f, 0.88f, 0.92f, 1.0f),
                  "MOVE (X: %.0f, Y: %.0f)",
                  layer_view.offset_x, layer_view.offset_y);
            break;
        case ActiveTool::Rotate:
            badge(ImVec4(0.95f, 0.84f, 0.35f, 1.0f),
                  "ROTATE (%.2f deg)",
                  gui.m_last_angle);
            break;
        case ActiveTool::Crop: {
            const auto rect = gui.get_crop_state().get_pixel_rect(img_w, img_h);
            badge(ImVec4(0.85f, 0.90f, 0.95f, 1.0f),
                  "CROP (%dx%d px)", rect.w, rect.h);
            break;
        }
        case ActiveTool::RasterErase: {
            const auto &re = gui.get_raster_eraser();
            badge(ImVec4(0.90f, 0.75f, 0.45f, 1.0f),
                  "ERASER (%.0fpx | H:%.0f%% | O:%.0f%%)",
                  re.brush_size, re.brush_hardness * 100.0f, re.brush_opacity * 100.0f);
            break;
        }
        case ActiveTool::AiInpaint:
            badge(ImVec4(0.95f, 0.50f, 0.50f, 1.0f),
                  "AI OBJECT ERASE (%s, %.0fpx)",
                  gui.is_erase_subtract() ? "Sub -" : "Add +", gui.m_erase_radius);
            break;
        case ActiveTool::Text:
            badge(ImVec4(0.65f, 0.82f, 1.00f, 1.0f), "TEXT TOOL");
            break;
        case ActiveTool::Upscale:
            badge(ImVec4(0.60f, 0.92f, 0.72f, 1.0f), "AI UPSCALER");
            break;
        default:
            break;
        }

        const float btn_area_w = 138.0f;
        const float right_pos = ImGui::GetWindowWidth() - btn_area_w;
        if (right_pos > ImGui::GetCursorPosX() + 10.0f) {
            ImGui::SameLine(right_pos);
        } else {
            ImGui::SameLine();
        }

        if (ImGui::SmallButton("Fit")) gui.m_req_fit_view = true;
        gui.register_dom_last_item("status_btn_fit", "Fit", "button", false, true);
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::SmallButton("100%")) gui.m_req_100_view = true;
        gui.register_dom_last_item("status_btn_100", "100%", "button", false, true);
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::SmallButton("200%")) gui.m_req_200_view = true;
        gui.register_dom_last_item("status_btn_200", "200%", "button", false, true);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

} // namespace gui
