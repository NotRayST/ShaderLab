#include "canvas.h"
#include "gui_manager.h"
#include "../erase_tool.h"
#include <imgui.h>
#include <algorithm>

namespace gui {

void render_center_canvas(GuiManager &gui, float left_offset, float top_offset, float canvas_width, float canvas_height,
                          uint32_t img_w, uint32_t img_h,
                          float /*zoom_level*/, float /*pan_x*/, float /*pan_y*/) {
    if (canvas_width <= 1.0f || canvas_height <= 1.0f) return;

    ImDrawList *fg = ImGui::GetBackgroundDrawList();

    // 1. splitter lines on left and right borders of canvas are drawn directly in panel windows
    // to prevent popup occlusion and ensure active highlight lines are always visible.

    // 2. before / after split divider line & handle
    if (gui.m_active_tool == ActiveTool::Split && gui.m_before_after && img_w > 0 && img_h > 0) {
        float split_norm = std::clamp(0.5f + gui.m_before_after_split, 0.05f, 0.95f);
        float split_x = left_offset + canvas_width * split_norm;
        float canvas_top = top_offset;
        float canvas_bot = top_offset + canvas_height;

        fg->AddLine(ImVec2(split_x, canvas_top), ImVec2(split_x, canvas_bot), IM_COL32(255, 255, 255, 210), 2.0f);

        float handle_y = canvas_top + canvas_height * 0.5f;
        fg->AddCircleFilled(ImVec2(split_x, handle_y), 12.0f, IM_COL32(28, 28, 30, 240));
        fg->AddCircle(ImVec2(split_x, handle_y), 12.0f, IM_COL32(255, 255, 255, 220), 24, 1.5f);
        fg->AddText(ImVec2(split_x - 11.0f, handle_y - 7.0f), IM_COL32(230, 230, 230, 255), "< | >");
    }

    // 3. erase brush mask (EraseTool GPU texture quad is the single source of truth)
    EraseTool &erase_tool = EraseTool::get();
    if (gui.m_device && img_w > 0 && img_h > 0 && erase_tool.is_mask_dirty()) {
        erase_tool.update_gpu_texture(gui.m_device);
    }
    bool has_tool_mask = erase_tool.has_mask_edits();
    bool show_mask = (gui.m_active_tool == ActiveTool::Erase) ||
                     (has_tool_mask && gui.m_active_tool == ActiveTool::Hand);
    if (show_mask && img_w > 0 && img_h > 0) {
        float fit_scale = (img_w > 0 && img_h > 0) ?
            (std::min)(canvas_width / static_cast<float>(img_w), canvas_height / static_cast<float>(img_h)) : 1.0f;
        float eff_zoom = (std::max)(0.0001f, gui.m_last_zoom * fit_scale);
        float cx = left_offset + canvas_width * 0.5f;
        float cy = top_offset + canvas_height * 0.5f;

        auto uv_to_screen = [&](const ImVec2 &uv) -> ImVec2 {
            float qx = (uv.x * img_w) - (img_w * 0.5f) - gui.m_last_pan_x;
            float qy = (uv.y * img_h) - (img_h * 0.5f) - gui.m_last_pan_y;
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

            ID3D11ShaderResourceView *mask_srv = erase_tool.get_mask_srv();
            if (mask_srv && has_tool_mask) {
                ImVec2 p0 = uv_to_screen(ImVec2(0.0f, 0.0f));
                ImVec2 p1 = uv_to_screen(ImVec2(1.0f, 0.0f));
                ImVec2 p2 = uv_to_screen(ImVec2(1.0f, 1.0f));
                ImVec2 p3 = uv_to_screen(ImVec2(0.0f, 1.0f));
                ImU32 quad_col = (gui.m_active_tool == ActiveTool::Erase)
                    ? IM_COL32(255, 255, 255, 255)
                    : IM_COL32(255, 255, 255, 140);
                fg->AddImageQuad((ImTextureID)mask_srv, p0, p1, p2, p3,
                                 ImVec2(0.0f, 0.0f), ImVec2(1.0f, 0.0f),
                                 ImVec2(1.0f, 1.0f), ImVec2(0.0f, 1.0f), quad_col);
            }

            fg->PopClipRect();
        }

        // Cursor ring: exact size match to painted stroke
        if (gui.m_active_tool == ActiveTool::Erase) {
            POINT mouse_pt = gui.m_last_mouse_pt;
            POINT os_pt = {};
            if (GetCursorPos(&os_pt) && ScreenToClient(gui.m_hwnd, &os_pt)) {
                mouse_pt = os_pt;
            }
            if ((gui.m_cursor_in_client || gui.m_is_painting_mask) &&
                mouse_pt.x >= left_offset && mouse_pt.x <= left_offset + canvas_width &&
                mouse_pt.y >= top_offset && mouse_pt.y <= top_offset + canvas_height) {
                bool alt_held = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                bool is_sub = gui.m_erase_subtract || alt_held;

                float screen_radius = gui.m_erase_radius * eff_zoom;
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
            if (open_file_dialog(gui.m_hwnd, L"Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files (*.*)\0*.*\0", path)) {
                gui.m_req_open_image = true;
                gui.m_open_image_path = path;
            }
        }
        gui.register_dom_last_item("empty_btn_open", "Open Image...", "button");

        ImGui::End();
    }
}

} // namespace gui
