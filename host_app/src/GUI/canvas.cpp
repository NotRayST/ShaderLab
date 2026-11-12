#include "canvas.h"
#include "gui_manager.h"
#include "../erase_tool.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gui {

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct CanvasProjection {
    float cx = 0.0f;
    float cy = 0.0f;
    float eff_zoom = 1.0f;
    float half_w = 0.0f;
    float half_h = 0.0f;
    float pan_x = 0.0f;
    float pan_y = 0.0f;
    float cos_a = 1.0f;
    float sin_a = 0.0f;

    ImVec2 doc_to_screen(float dx, float dy) const {
        const float qx = dx - (half_w + pan_x);
        const float qy = dy - (half_h + pan_y);
        const float px = qx * cos_a - qy * sin_a;
        const float py = qx * sin_a + qy * cos_a;
        return ImVec2(cx + px * eff_zoom, cy + py * eff_zoom);
    }
};

CanvasProjection build_projection(float left_offset, float top_offset, float canvas_w, float canvas_h,
                                  uint32_t img_w, uint32_t img_h,
                                  float zoom_level, float pan_x, float pan_y, float angle_deg) {
    CanvasProjection proj;
    proj.cx = left_offset + canvas_w * 0.5f;
    proj.cy = top_offset + canvas_h * 0.5f;
    proj.half_w = static_cast<float>(img_w) * 0.5f;
    proj.half_h = static_cast<float>(img_h) * 0.5f;
    proj.pan_x = pan_x;
    proj.pan_y = pan_y;

    const float rad = angle_deg * (kPi / 180.0f);
    proj.cos_a = std::cos(rad);
    proj.sin_a = std::sin(rad);

    if (img_w > 0 && img_h > 0) {
        const float abs_c = std::abs(proj.cos_a);
        const float abs_s = std::abs(proj.sin_a);
        const float rot_w = static_cast<float>(img_w) * abs_c + static_cast<float>(img_h) * abs_s;
        const float rot_h = static_cast<float>(img_w) * abs_s + static_cast<float>(img_h) * abs_c;
        const float fit_scale = (std::min)(
            canvas_w / (std::max)(1.0f, rot_w),
            canvas_h / (std::max)(1.0f, rot_h));
        proj.eff_zoom = (std::max)(0.001f, zoom_level * fit_scale);
    }
    return proj;
}

void draw_square_handle(ImDrawList *bg, ImVec2 center, float half_sz, ImU32 fill_col, ImU32 border_col) {
    bg->AddRectFilled(ImVec2(center.x - half_sz, center.y - half_sz),
                      ImVec2(center.x + half_sz, center.y + half_sz), fill_col);
    bg->AddRect(ImVec2(center.x - half_sz, center.y - half_sz),
                ImVec2(center.x + half_sz, center.y + half_sz), border_col, 0.0f, 0, 1.5f);
}

} // namespace

void render_center_canvas(GuiManager &gui, float left_offset, float top_offset,
                          float canvas_width, float canvas_height,
                          uint32_t img_w, uint32_t img_h,
                          float zoom_level, float pan_x, float pan_y) {
    // Store canvas bounds so input_hook & simulate_canvas_mouse match exact viewport geometry
    gui.m_canvas_view_x = left_offset;
    gui.m_canvas_view_y = top_offset;
    gui.m_canvas_view_w = canvas_width;
    gui.m_canvas_view_h = canvas_height;
    gui.m_last_img_w = img_w;
    gui.m_last_img_h = img_h;
    gui.m_last_zoom = zoom_level;
    gui.m_last_pan_x = pan_x;
    gui.m_last_pan_y = pan_y;

    const ImVec2 canvas_min(left_offset, top_offset);
    const ImVec2 canvas_max(left_offset + canvas_width, top_offset + canvas_height);

    // IMPORTANT (AGENTS.MD): Never use ImGui::GetForegroundDrawList().
    // Render canvas overlays cleanly on GetBackgroundDrawList clipped to the center viewport.
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    bg->PushClipRect(canvas_min, canvas_max, true);

    if (img_w > 0 && img_h > 0) {
        const CanvasProjection proj = build_projection(
            left_offset, top_offset, canvas_width, canvas_height,
            img_w, img_h, zoom_level, pan_x, pan_y, gui.m_last_angle);

        const float fw = static_cast<float>(img_w);
        const float fh = static_cast<float>(img_h);
        const ImVec2 doc_tl = proj.doc_to_screen(0.0f, 0.0f);
        const ImVec2 doc_tr = proj.doc_to_screen(fw, 0.0f);
        const ImVec2 doc_br = proj.doc_to_screen(fw, fh);
        const ImVec2 doc_bl = proj.doc_to_screen(0.0f, fh);

        // Subtle crisp outline around the Document Frame
        bg->AddQuad(doc_tl, doc_tr, doc_br, doc_bl, IM_COL32(255, 255, 255, 42), 1.0f);

        // 1. AI Object Erase mask overlay (Feature #7 / #12)
        EraseTool &erase_tool = EraseTool::get();
        const bool has_tool_mask = erase_tool.has_mask_edits();
        const bool show_mask = (gui.m_active_tool == ActiveTool::AiInpaint) ||
                               (has_tool_mask && gui.m_active_tool == ActiveTool::Hand);
        if (show_mask && has_tool_mask) {
            if (erase_tool.is_mask_dirty() && gui.m_device) {
                erase_tool.update_gpu_texture(gui.m_device);
            }
            if (ID3D11ShaderResourceView *mask_srv = erase_tool.get_mask_srv()) {
                bg->AddImageQuad(
                    reinterpret_cast<ImTextureID>(mask_srv),
                    doc_tl, doc_tr, doc_br, doc_bl,
                    ImVec2(0.0f, 0.0f), ImVec2(1.0f, 0.0f), ImVec2(1.0f, 1.0f), ImVec2(0.0f, 1.0f),
                    IM_COL32(255, 68, 68, 145));
            }
        }

        // 2. Move Tool layer bounding box + 8 transform handles (Feature #8)
        if (gui.m_active_tool == ActiveTool::Move && gui.m_show_layer_bounds) {
            const auto &lv = gui.get_active_layer_view();
            const float lx0 = lv.offset_x;
            const float ly0 = lv.offset_y;
            const float lw = (lv.valid && lv.width > 0) ? static_cast<float>(lv.width) : fw;
            const float lh = (lv.valid && lv.height > 0) ? static_cast<float>(lv.height) : fh;
            const float lx1 = lx0 + lw;
            const float ly1 = ly0 + lh;

            const ImVec2 q_tl = proj.doc_to_screen(lx0, ly0);
            const ImVec2 q_tr = proj.doc_to_screen(lx1, ly0);
            const ImVec2 q_br = proj.doc_to_screen(lx1, ly1);
            const ImVec2 q_bl = proj.doc_to_screen(lx0, ly1);
            const ImVec2 q_tm = proj.doc_to_screen((lx0 + lx1) * 0.5f, ly0);
            const ImVec2 q_rm = proj.doc_to_screen(lx1, (ly0 + ly1) * 0.5f);
            const ImVec2 q_bm = proj.doc_to_screen((lx0 + lx1) * 0.5f, ly1);
            const ImVec2 q_lm = proj.doc_to_screen(lx0, (ly0 + ly1) * 0.5f);

            const ImU32 cyan = lv.lock_position ? IM_COL32(235, 130, 80, 220) : IM_COL32(60, 215, 240, 235);
            bg->AddQuad(q_tl, q_tr, q_br, q_bl, cyan, 1.5f);
            for (const ImVec2 &hpt : { q_tl, q_tr, q_br, q_bl, q_tm, q_rm, q_bm, q_lm }) {
                draw_square_handle(bg, hpt, 3.5f, IM_COL32(255, 255, 255, 255), cyan);
            }
        }

        // 3. Rotate & Camera Raw Horizon Align Overlay (Feature #6)
        if (gui.m_active_tool == ActiveTool::Rotate) {
            if (gui.m_horizon_state.is_drawing_line || gui.m_is_dragging_angle_slider) {
                constexpr int kGridDivs = 12;
                for (int i = 1; i < kGridDivs; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(kGridDivs);
                    const float gx = left_offset + canvas_width * t;
                    const float gy = top_offset + canvas_height * t;
                    bg->AddLine(ImVec2(gx, top_offset), ImVec2(gx, top_offset + canvas_height), IM_COL32(255, 255, 255, 55), 1.0f);
                    bg->AddLine(ImVec2(left_offset, gy), ImVec2(left_offset + canvas_width, gy), IM_COL32(255, 255, 255, 55), 1.0f);
                }
            }

            if (gui.m_horizon_state.is_drawing_line) {
                const ImVec2 p1 = gui.m_horizon_state.start_screen;
                const ImVec2 p2 = gui.m_horizon_state.end_screen;
                bg->AddLine(p1, p2, IM_COL32(0, 0, 0, 200), 3.5f);
                bg->AddLine(p1, p2, IM_COL32(255, 210, 60, 255), 1.8f);

                bg->AddCircleFilled(p1, 4.5f, IM_COL32(255, 255, 255, 255));
                bg->AddCircle(p1, 4.5f, IM_COL32(0, 0, 0, 220), 16, 1.5f);
                bg->AddCircleFilled(p2, 4.5f, IM_COL32(255, 255, 255, 255));
                bg->AddCircle(p2, 4.5f, IM_COL32(0, 0, 0, 220), 16, 1.5f);

                const float delta_deg = HorizonAlignState::compute_horizon_straighten_angle(p1, p2);
                char badge[64] = {};
                std::snprintf(badge, sizeof(badge), "Horizon: %+.2f deg", delta_deg);
                ImVec2 mid((p1.x + p2.x) * 0.5f + 10.0f, (p1.y + p2.y) * 0.5f - 26.0f);
                const ImVec2 bsz = ImGui::CalcTextSize(badge);
                mid.x = std::clamp(mid.x, left_offset + 6.0f, left_offset + canvas_width - bsz.x - 14.0f);
                mid.y = std::clamp(mid.y, top_offset + 6.0f, top_offset + canvas_height - bsz.y - 10.0f);
                bg->AddRectFilled(ImVec2(mid.x - 6.0f, mid.y - 4.0f),
                                  ImVec2(mid.x + bsz.x + 6.0f, mid.y + bsz.y + 4.0f),
                                  IM_COL32(20, 20, 24, 232), 4.0f);
                bg->AddText(mid, IM_COL32(255, 215, 70, 255), badge);
            }
        }

        // 4. Crop Tool Overlay (Feature #13): 65% black exterior shield, white border, Rule-of-Thirds, 8 handles
        if (gui.m_active_tool == ActiveTool::Crop) {
            const auto &cs = gui.get_crop_state();
            const float dx0 = cs.x0 * fw;
            const float dy0 = cs.y0 * fh;
            const float dx1 = cs.x1 * fw;
            const float dy1 = cs.y1 * fh;

            const ImVec2 c_tl = proj.doc_to_screen(dx0, dy0);
            const ImVec2 c_tr = proj.doc_to_screen(dx1, dy0);
            const ImVec2 c_br = proj.doc_to_screen(dx1, dy1);
            const ImVec2 c_bl = proj.doc_to_screen(dx0, dy1);

            constexpr ImU32 kShield = IM_COL32(0, 0, 0, 166); // ~65% dim shield
            if (std::abs(gui.m_last_angle) < 0.01f) {
                const float sx0 = c_tl.x;
                const float sy0 = c_tl.y;
                const float sx1 = c_br.x;
                const float sy1 = c_br.y;
                bg->AddRectFilled(canvas_min, ImVec2(canvas_max.x, sy0), kShield);
                bg->AddRectFilled(ImVec2(canvas_min.x, sy1), canvas_max, kShield);
                bg->AddRectFilled(ImVec2(canvas_min.x, sy0), ImVec2(sx0, sy1), kShield);
                bg->AddRectFilled(ImVec2(sx1, sy0), ImVec2(canvas_max.x, sy1), kShield);
            } else {
                bg->AddQuadFilled(doc_tl, doc_tr, c_tr, c_tl, kShield);
                bg->AddQuadFilled(c_tr, doc_tr, doc_br, c_br, kShield);
                bg->AddQuadFilled(c_bl, c_br, doc_br, doc_bl, kShield);
                bg->AddQuadFilled(doc_tl, c_tl, c_bl, doc_bl, kShield);
            }

            // Rule of Thirds lines (2 vertical, 2 horizontal)
            for (int k = 1; k <= 2; ++k) {
                const float t = static_cast<float>(k) / 3.0f;
                const float vx = dx0 + (dx1 - dx0) * t;
                const float hy = dy0 + (dy1 - dy0) * t;
                bg->AddLine(proj.doc_to_screen(vx, dy0), proj.doc_to_screen(vx, dy1), IM_COL32(255, 255, 255, 115), 1.0f);
                bg->AddLine(proj.doc_to_screen(dx0, hy), proj.doc_to_screen(dx1, hy), IM_COL32(255, 255, 255, 115), 1.0f);
            }

            // Crisp crop border
            bg->AddQuad(c_tl, c_tr, c_br, c_bl, IM_COL32(0, 0, 0, 140), 3.0f);
            bg->AddQuad(c_tl, c_tr, c_br, c_bl, IM_COL32(255, 255, 255, 245), 1.5f);

            // 8 thick corner L-brackets & edge handles
            auto draw_bracket = [&](ImVec2 corner, ImVec2 dir_x, ImVec2 dir_y) {
                const float len = 14.0f;
                const ImVec2 pts[3] = {
                    ImVec2(corner.x + dir_x.x * len, corner.y + dir_x.y * len),
                    corner,
                    ImVec2(corner.x + dir_y.x * len, corner.y + dir_y.y * len)
                };
                bg->AddPolyline(pts, 3, IM_COL32(0, 0, 0, 180), ImDrawFlags_None, 5.0f);
                bg->AddPolyline(pts, 3, IM_COL32(255, 255, 255, 255), ImDrawFlags_None, 3.0f);
            };
            auto norm_dir = [](ImVec2 a, ImVec2 b) -> ImVec2 {
                const float vx = b.x - a.x;
                const float vy = b.y - a.y;
                const float len = std::hypot(vx, vy);
                return (len > 1e-4f) ? ImVec2(vx / len, vy / len) : ImVec2(1.0f, 0.0f);
            };
            const ImVec2 u_r = norm_dir(c_tl, c_tr);
            const ImVec2 u_d = norm_dir(c_tl, c_bl);
            draw_bracket(c_tl, u_r, u_d);
            draw_bracket(c_tr, ImVec2(-u_r.x, -u_r.y), u_d);
            draw_bracket(c_br, ImVec2(-u_r.x, -u_r.y), ImVec2(-u_d.x, -u_d.y));
            draw_bracket(c_bl, u_r, ImVec2(-u_d.x, -u_d.y));

            auto draw_edge_bar = [&](ImVec2 a, ImVec2 b, ImVec2 tang) {
                const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                const ImVec2 p0(mid.x - tang.x * 10.0f, mid.y - tang.y * 10.0f);
                const ImVec2 p1(mid.x + tang.x * 10.0f, mid.y + tang.y * 10.0f);
                bg->AddLine(p0, p1, IM_COL32(0, 0, 0, 180), 5.0f);
                bg->AddLine(p0, p1, IM_COL32(255, 255, 255, 255), 3.0f);
            };
            draw_edge_bar(c_tl, c_tr, u_r);
            draw_edge_bar(c_tr, c_br, u_d);
            draw_edge_bar(c_bl, c_br, u_r);
            draw_edge_bar(c_tl, c_bl, u_d);

            // Pixel dimension readout badge
            const auto prect = cs.get_pixel_rect(img_w, img_h);
            char ctxt[48] = {};
            std::snprintf(ctxt, sizeof(ctxt), "%d x %d px", prect.w, prect.h);
            const ImVec2 tsz = ImGui::CalcTextSize(ctxt);
            ImVec2 bpos(c_br.x - tsz.x - 10.0f, c_br.y + 8.0f);
            bpos.x = std::clamp(bpos.x, left_offset + 6.0f, left_offset + canvas_width - tsz.x - 12.0f);
            bpos.y = std::clamp(bpos.y, top_offset + 6.0f, top_offset + canvas_height - tsz.y - 10.0f);
            bg->AddRectFilled(ImVec2(bpos.x - 5.0f, bpos.y - 3.0f),
                              ImVec2(bpos.x + tsz.x + 5.0f, bpos.y + tsz.y + 3.0f),
                              IM_COL32(20, 20, 24, 225), 3.0f);
            bg->AddText(bpos, IM_COL32(240, 244, 248, 255), ctxt);
        }

        // 5. Text Tool caret / bounding box preview hook (Feature #15)
        const auto &tprev = gui.get_text_caret_preview();
        if (gui.m_active_tool == ActiveTool::Text && tprev.active) {
            const ImVec2 t_tl = proj.doc_to_screen(tprev.doc_x, tprev.doc_y);
            const ImVec2 t_tr = proj.doc_to_screen(tprev.doc_x + tprev.doc_w, tprev.doc_y);
            const ImVec2 t_br = proj.doc_to_screen(tprev.doc_x + tprev.doc_w, tprev.doc_y + tprev.doc_h);
            const ImVec2 t_bl = proj.doc_to_screen(tprev.doc_x, tprev.doc_y + tprev.doc_h);
            bg->AddQuad(t_tl, t_tr, t_br, t_bl, IM_COL32(115, 195, 255, 200), 1.2f);
            if (tprev.editing) {
                bg->AddLine(t_tr, t_br, IM_COL32(255, 255, 255, 230), 1.5f);
            }
        }

        // 6. Split Before / After divider line (Feature B)
        if (gui.m_before_after) {
            const float split_norm = std::clamp(0.5f + gui.m_before_after_split, 0.05f, 0.95f);
            const float sx = left_offset + canvas_width * split_norm;
            bg->AddLine(ImVec2(sx, top_offset), ImVec2(sx, top_offset + canvas_height), IM_COL32(0, 0, 0, 170), 3.0f);
            bg->AddLine(ImVec2(sx, top_offset), ImVec2(sx, top_offset + canvas_height), IM_COL32(245, 165, 65, 255), 1.5f);

            const float mid_y = top_offset + canvas_height * 0.5f;
            bg->AddCircleFilled(ImVec2(sx, mid_y), 11.0f, IM_COL32(30, 30, 36, 235));
            bg->AddCircle(ImVec2(sx, mid_y), 11.0f, IM_COL32(245, 165, 65, 255), 20, 1.5f);
            bg->AddText(ImVec2(sx - 6.0f, mid_y - 7.0f), IM_COL32(255, 255, 255, 255), "<>");
        }

        // 7. Custom brush rings on canvas (Feature #7 & Feature #12)
        const bool space_held = gui.m_space_held || ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0);
        const bool popup_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        const ImVec2 mouse_pos = ImGui::GetMousePos();
        const bool mouse_in_canvas =
            mouse_pos.x >= left_offset && mouse_pos.x <= left_offset + canvas_width &&
            mouse_pos.y >= top_offset && mouse_pos.y <= top_offset + canvas_height;

        if (mouse_in_canvas && !space_held && !popup_open) {
            if (gui.m_active_tool == ActiveTool::AiInpaint) {
                const bool sub_mode = gui.is_erase_subtract() ||
                                      ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) ||
                                      ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
                const float r_scr = (std::max)(3.0f, gui.m_erase_radius * proj.eff_zoom);
                const ImU32 ring_col = sub_mode ? IM_COL32(255, 215, 65, 255) : IM_COL32(255, 255, 255, 255);
                bg->AddCircle(mouse_pos, r_scr, IM_COL32(0, 0, 0, 210), 48, 2.8f);
                bg->AddCircle(mouse_pos, r_scr, ring_col, 48, 1.5f);
                if (sub_mode) {
                    bg->AddLine(ImVec2(mouse_pos.x - 5.0f, mouse_pos.y), ImVec2(mouse_pos.x + 5.0f, mouse_pos.y), ring_col, 2.0f);
                } else {
                    bg->AddLine(ImVec2(mouse_pos.x - 4.0f, mouse_pos.y), ImVec2(mouse_pos.x + 4.0f, mouse_pos.y), ring_col, 1.5f);
                    bg->AddLine(ImVec2(mouse_pos.x, mouse_pos.y - 4.0f), ImVec2(mouse_pos.x, mouse_pos.y + 4.0f), ring_col, 1.5f);
                }
            } else if (gui.m_active_tool == ActiveTool::RasterErase) {
                const float r_scr = (std::max)(2.0f, (gui.m_raster_eraser.brush_size * 0.5f) * proj.eff_zoom);
                const float r_hard = r_scr * std::clamp(gui.m_raster_eraser.brush_hardness, 0.0f, 1.0f);
                bg->AddCircle(mouse_pos, r_scr, IM_COL32(0, 0, 0, 200), 48, 2.5f);
                bg->AddCircle(mouse_pos, r_scr, IM_COL32(255, 255, 255, 240), 48, 1.4f);
                if (r_hard > 2.0f && r_hard < r_scr - 1.5f) {
                    bg->AddCircle(mouse_pos, r_hard, IM_COL32(255, 255, 255, 95), 40, 1.0f);
                }
                bg->AddCircleFilled(mouse_pos, 1.5f, IM_COL32(255, 255, 255, 220));
            }
        }
    }

    bg->PopClipRect();

    // Empty-canvas welcome overlay when no image is loaded
    if (img_w == 0 || img_h == 0) {
        const ImVec2 win_center(left_offset + canvas_width * 0.5f, top_offset + canvas_height * 0.5f);
        ImGui::SetNextWindowPos(win_center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2((std::min)(360.0f, canvas_width - 24.0f), 0.0f));
        constexpr ImGuiWindowFlags kEmptyFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 16.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        if (ImGui::Begin("##CanvasEmptyOverlay", nullptr, kEmptyFlags)) {
            ImGui::TextUnformatted("ShaderLab Studio");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextWrapped("Drag & drop an image or .shaderlab project here, or use File -> Open Image or Project (Ctrl+O).");
            ImGui::Spacing();
            if (ImGui::Button("Open Image / Project...", ImVec2(-1.0f, 28.0f))) {
                gui.m_req_open_image = false;
                // Trigger standard Ctrl+O path via menu/IPC or keyboard simulation if needed
                wchar_t file_buf[MAX_PATH] = {};
                OPENFILENAMEW ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = gui.get_hwnd();
                ofn.lpstrFilter = L"Supported Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.shaderlab\0All Files\0*.*\0";
                ofn.lpstrFile = file_buf;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    gui.m_open_image_path = file_buf;
                    gui.m_req_open_image = true;
                }
            }
            gui.register_dom_last_item("canvas_btn_open", "Open Image / Project...", "button", false, true);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

} // namespace gui
