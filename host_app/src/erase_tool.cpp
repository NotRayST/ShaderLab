#include "erase_tool.h"
#include "depth_manager.h"
#include "../../common/depth_file.h"
#include "../../common/str_utils.h"
#include "../../third_party/stb/stb_image.h"
#include "../../third_party/stb/stb_image_write.h"

#include <windows.h>
#include <urlmon.h>
#pragma comment(lib, "urlmon.lib")
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
using str_utils::wide_to_utf8;
using str_utils::utf8_to_wide;

EraseTool &EraseTool::get() { static EraseTool instance; return instance; }
EraseTool::EraseTool() = default;
EraseTool::~EraseTool() {
    m_cancel_download.store(true);
    if (m_download_thread.joinable()) m_download_thread.join();
    stop_worker();
    if (m_worker.joinable()) m_worker.join();
}

void EraseTool::set_active(bool active) {
    m_active = active; m_is_drawing = false;
    if (active) prewarm_worker();
}

void EraseTool::clear_mask() {
    if (!m_mask.empty()) std::fill(m_mask.begin(), m_mask.end(), (uint8_t)0);
    m_mask_white_px = 0; m_is_drawing = false;
    m_undo_stack.clear(); m_redo_stack.clear();
    mark_dirty_all();
}

bool EraseTool::undo_stroke() {
    if (m_undo_stack.empty()) return false;
    m_redo_stack.push_back(std::move(m_mask));
    m_mask = std::move(m_undo_stack.back()); m_undo_stack.pop_back();
    recalculate_white_pixels(); mark_dirty_all(); return true;
}

bool EraseTool::redo_stroke() {
    if (m_redo_stack.empty()) return false;
    m_undo_stack.push_back(std::move(m_mask));
    m_mask = std::move(m_redo_stack.back()); m_redo_stack.pop_back();
    recalculate_white_pixels(); mark_dirty_all(); return true;
}

void EraseTool::ensure_mask_size(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || (m_mask_w == w && m_mask_h == h && m_mask.size() == (size_t)w * h)) return;
    m_mask_w = w; m_mask_h = h; m_mask.assign((size_t)w * h, 0);
    m_mask_white_px = 0; m_undo_stack.clear(); m_redo_stack.clear(); mark_dirty_all();
}

void EraseTool::push_undo() {
    if (m_mask.empty()) return;
    if (m_undo_stack.size() >= 15) m_undo_stack.erase(m_undo_stack.begin());
    m_undo_stack.push_back(m_mask); m_redo_stack.clear();
}

void EraseTool::recalculate_white_pixels() {
    size_t count = 0;
    for (uint8_t v : m_mask) { if (v > 120) count++; }
    m_mask_white_px = count;
}

void EraseTool::mark_dirty_rect(int x0, int y0, int x1, int y1) {
    if (m_mask_w == 0 || m_mask_h == 0) return;
    x0 = std::clamp(x0, 0, static_cast<int>(m_mask_w) - 1);
    y0 = std::clamp(y0, 0, static_cast<int>(m_mask_h) - 1);
    x1 = std::clamp(x1, 0, static_cast<int>(m_mask_w) - 1);
    y1 = std::clamp(y1, 0, static_cast<int>(m_mask_h) - 1);
    if (x0 > x1 || y0 > y1) return;
    if (!m_has_dirty_rect) {
        m_dirty_x0 = x0;
        m_dirty_y0 = y0;
        m_dirty_x1 = x1;
        m_dirty_y1 = y1;
        m_has_dirty_rect = true;
    } else {
        m_dirty_x0 = (std::min)(m_dirty_x0, x0);
        m_dirty_y0 = (std::min)(m_dirty_y0, y0);
        m_dirty_x1 = (std::max)(m_dirty_x1, x1);
        m_dirty_y1 = (std::max)(m_dirty_y1, y1);
    }
    m_mask_dirty = true;
}

void EraseTool::mark_dirty_all() {
    if (m_mask_w == 0 || m_mask_h == 0) {
        m_has_dirty_rect = false;
        m_mask_dirty = true;
        return;
    }
    m_dirty_x0 = 0;
    m_dirty_y0 = 0;
    m_dirty_x1 = static_cast<int>(m_mask_w) - 1;
    m_dirty_y1 = static_cast<int>(m_mask_h) - 1;
    m_has_dirty_rect = true;
    m_mask_dirty = true;
}

void EraseTool::draw_brush_dab(int cx, int cy, int r, uint8_t val) {
    if (m_mask.empty() || m_mask_w == 0 || m_mask_h == 0) return;
    if (r < 1) r = 1;
    int y0 = (std::max)(0, cy - r), y1 = (std::min)((int)m_mask_h - 1, cy + r), r2 = r * r;
    int min_x = (int)m_mask_w, max_x = -1;
    const bool paint = (val > 120);
    for (int y = y0; y <= y1; ++y) {
        int dy = y - cy, max_dx = (int)std::sqrt((std::max)(0, r2 - dy * dy));
        int x0 = (std::max)(0, cx - max_dx), x1 = (std::min)((int)m_mask_w - 1, cx + max_dx);
        if (x0 <= x1) {
            min_x = (std::min)(min_x, x0);
            max_x = (std::max)(max_x, x1);
            uint8_t *row = m_mask.data() + (size_t)y * m_mask_w;
            if (paint) {
                for (int x = x0; x <= x1; ++x) {
                    if (row[x] <= 120) ++m_mask_white_px;
                    row[x] = val;
                }
            } else {
                for (int x = x0; x <= x1; ++x) {
                    if (row[x] > 120 && m_mask_white_px > 0) --m_mask_white_px;
                    row[x] = val;
                }
            }
        }
    }
    if (min_x <= max_x && y0 <= y1) {
        mark_dirty_rect(min_x, y0, max_x, y1);
    }
}

void EraseTool::draw_brush_line(const ImVec2 &p0, const ImVec2 &p1, float radius, bool is_sub) {
    if (m_mask.empty() || m_mask_w == 0 || m_mask_h == 0) return;
    const float r = (std::max)(1.0f, radius);
    const float r2 = r * r;
    const uint8_t val = is_sub ? 0 : 255;

    const int x0 = (std::max)(0, static_cast<int>(std::floor((std::min)(p0.x, p1.x) - r)));
    const int y0 = (std::max)(0, static_cast<int>(std::floor((std::min)(p0.y, p1.y) - r)));
    const int x1 = (std::min)(static_cast<int>(m_mask_w) - 1, static_cast<int>(std::ceil((std::max)(p0.x, p1.x) + r)));
    const int y1 = (std::min)(static_cast<int>(m_mask_h) - 1, static_cast<int>(std::ceil((std::max)(p0.y, p1.y) + r)));
    if (x0 > x1 || y0 > y1) return;

    const float abx = p1.x - p0.x;
    const float aby = p1.y - p0.y;
    const float ab_len2 = abx * abx + aby * aby;
    const bool single_pt = (ab_len2 <= 1e-6f);

    for (int y = y0; y <= y1; ++y) {
        uint8_t *row = m_mask.data() + static_cast<size_t>(y) * m_mask_w;
        const float apy = static_cast<float>(y) - p0.y;
        for (int x = x0; x <= x1; ++x) {
            const float apx = static_cast<float>(x) - p0.x;
            float dx = apx;
            float dy = apy;
            if (!single_pt) {
                const float t = std::clamp((apx * abx + apy * aby) / ab_len2, 0.0f, 1.0f);
                dx = apx - abx * t;
                dy = apy - aby * t;
            }
            if (dx * dx + dy * dy <= r2) {
                if (!is_sub) {
                    if (row[x] <= 120) ++m_mask_white_px;
                } else {
                    if (row[x] > 120 && m_mask_white_px > 0) --m_mask_white_px;
                }
                row[x] = val;
            }
        }
    }
    mark_dirty_rect(x0, y0, x1, y1);
}

static void compute_view_geom(const SharedControlBlock *b, const ImVec2 &disp, float &cos_a, float &sin_a, float &eff_zoom) {
    float img_w = (float)b->view_image_width, img_h = (float)b->view_image_height, user_zoom = b->view_zoom > 0.0f ? b->view_zoom : 1.0f;
    float rad = b->view_angle * (3.14159265358979323846f / 180.0f);
    cos_a = std::cos(rad); sin_a = std::sin(rad);
    float rw = (std::max)(std::fabs(cos_a * img_w) + std::fabs(sin_a * img_h), 1.0f);
    float rh = (std::max)(std::fabs(sin_a * img_w) + std::fabs(cos_a * img_h), 1.0f);
    eff_zoom = (std::max)(1e-6f, user_zoom * (std::min)(disp.x / rw, disp.y / rh));
}

ImVec2 EraseTool::screen_to_image(const ImVec2 &sp, const SharedControlBlock *b, const ImVec2 &disp) const {
    if (!b || b->view_image_width == 0 || disp.x <= 0) return sp;
    float c, s, z; compute_view_geom(b, disp, c, s, z);
    float px = (sp.x - disp.x * 0.5f) / z, py = (sp.y - disp.y * 0.5f) / z;
    return ImVec2(px * c + py * s + (float)b->view_image_width * 0.5f + (float)b->view_pan[0],
                  -px * s + py * c + (float)b->view_image_height * 0.5f + (float)b->view_pan[1]);
}

ImVec2 EraseTool::image_to_screen(const ImVec2 &ip, const SharedControlBlock *b, const ImVec2 &disp) const {
    if (!b || b->view_image_width == 0 || disp.x <= 0) return ip;
    float c, s, z; compute_view_geom(b, disp, c, s, z);
    float qx = ip.x - ((float)b->view_image_width * 0.5f + (float)b->view_pan[0]);
    float qy = ip.y - ((float)b->view_image_height * 0.5f + (float)b->view_pan[1]);
    return ImVec2((qx * c - qy * s) * z + disp.x * 0.5f, (qx * s + qy * c) * z + disp.y * 0.5f);
}

void EraseTool::handle_input(SharedControlBlock *block, bool is_processing, bool *out_stroke_finished) {
    if (out_stroke_finished) *out_stroke_finished = false;
    if (!block || block->view_image_width == 0) return;
    ensure_mask_size(block->view_image_width, block->view_image_height);
    if (!m_active || is_processing || (block->view_interaction_flags & VIEW_FLAG_BEFORE_AFTER) != 0) { m_is_drawing = false; return; }

    ImGuiIO &io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        set_active(false);
        return;
    }

    bool left = (io.MouseDown[0] || ImGui::IsMouseClicked(0)) && !io.KeyAlt && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    bool right = (io.MouseDown[1] || ImGui::IsMouseClicked(1)) && ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);

    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) brush_radius = (std::max)(2.0f, brush_radius - 4.0f);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) brush_radius = (std::min)(256.0f, brush_radius + 4.0f);

    if ((!left && !right) || ImGui::IsKeyDown(ImGuiKey_Space) || io.MouseDown[2] || (GetAsyncKeyState(VK_MBUTTON) & 0x8000)) {
        if (m_is_drawing) {
            m_is_drawing = false;
            recalculate_white_pixels();
            if (out_stroke_finished && !m_last_sub) *out_stroke_finished = true;
        }
        return;
    }

    bool in_ui = (io.MousePos.x >= m_ui_min.x && io.MousePos.x <= m_ui_max.x &&
                  io.MousePos.y >= m_ui_min.y && io.MousePos.y <= m_ui_max.y) ||
                 ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
    if (in_ui && !m_is_drawing) return;

    float img_w = (float)block->view_image_width, img_h = (float)block->view_image_height;
    ImVec2 ip = screen_to_image(io.MousePos, block, io.DisplaySize);
    if ((ip.x < 0 || ip.x >= img_w || ip.y < 0 || ip.y >= img_h) && !m_is_drawing) return;

    ImVec2 clamped(std::clamp(ip.x, 0.0f, img_w - 1.0f), std::clamp(ip.y, 0.0f, img_h - 1.0f));
    float cos_a, sin_a, eff_zoom; compute_view_geom(block, io.DisplaySize, cos_a, sin_a, eff_zoom);
    float rad_img = (std::max)(1.0f, brush_radius / eff_zoom);
    bool is_sub = subtract_mode || right;

    if (!m_is_drawing) {
        m_is_drawing = true; m_last_sub = is_sub; push_undo(); m_last_pos = clamped;
        draw_brush_dab((int)std::round(clamped.x), (int)std::round(clamped.y), (int)std::round(rad_img), is_sub ? 0 : 255);
        m_mask_dirty = true;
        if (!is_sub) m_mask_white_px = (std::max)(m_mask_white_px, (size_t)1);
    } else {
        draw_brush_line(m_last_pos, clamped, rad_img, is_sub); m_last_pos = clamped;
        if (!is_sub) m_mask_white_px = (std::max)(m_mask_white_px, (size_t)1);
    }
}

void EraseTool::update_gpu_texture(ID3D11Device *dev) {
    if (!dev || m_mask.empty() || !m_mask_dirty) return;
    bool full_upload = false;
    if (!m_mask_tex || m_tex_w != m_mask_w || m_tex_h != m_mask_h) {
        m_mask_tex.Reset(); m_mask_srv.Reset();
        D3D11_TEXTURE2D_DESC desc = { m_mask_w, m_mask_h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE };
        if (FAILED(dev->CreateTexture2D(&desc, nullptr, &m_mask_tex)) ||
            FAILED(dev->CreateShaderResourceView(m_mask_tex.Get(), nullptr, &m_mask_srv))) return;
        m_tex_w = m_mask_w; m_tex_h = m_mask_h;
        full_upload = true;
    }
    size_t n = (size_t)m_mask_w * m_mask_h;
    if (m_gpu_rgba.size() != n) {
        m_gpu_rgba.assign(n, 0);
        full_upload = true;
    }

    int ux0 = 0;
    int uy0 = 0;
    int ux1 = static_cast<int>(m_mask_w) - 1;
    int uy1 = static_cast<int>(m_mask_h) - 1;
    if (!full_upload && m_has_dirty_rect) {
        ux0 = std::clamp(m_dirty_x0, 0, static_cast<int>(m_mask_w) - 1);
        uy0 = std::clamp(m_dirty_y0, 0, static_cast<int>(m_mask_h) - 1);
        ux1 = std::clamp(m_dirty_x1, 0, static_cast<int>(m_mask_w) - 1);
        uy1 = std::clamp(m_dirty_y1, 0, static_cast<int>(m_mask_h) - 1);
    }

    if (ux0 <= ux1 && uy0 <= uy1) {
        const uint32_t mask_rgba = IM_COL32(245, 45, 45, 160);
        for (int y = uy0; y <= uy1; ++y) {
            const uint8_t *src_row = m_mask.data() + static_cast<size_t>(y) * m_mask_w;
            uint32_t *dst_row = m_gpu_rgba.data() + static_cast<size_t>(y) * m_mask_w;
            for (int x = ux0; x <= ux1; ++x) {
                dst_row[x] = (src_row[x] > 120) ? mask_rgba : 0;
            }
        }
        ComPtr<ID3D11DeviceContext> ctx; dev->GetImmediateContext(&ctx);
        if (ctx) {
            if (ux0 == 0 && uy0 == 0 && ux1 == static_cast<int>(m_mask_w) - 1 && uy1 == static_cast<int>(m_mask_h) - 1) {
                ctx->UpdateSubresource(m_mask_tex.Get(), 0, nullptr, m_gpu_rgba.data(), m_mask_w * sizeof(uint32_t), 0);
            } else {
                D3D11_BOX box = {};
                box.left = static_cast<UINT>(ux0);
                box.top = static_cast<UINT>(uy0);
                box.front = 0;
                box.right = static_cast<UINT>(ux1 + 1);
                box.bottom = static_cast<UINT>(uy1 + 1);
                box.back = 1;
                const uint32_t *src_ptr = m_gpu_rgba.data() + static_cast<size_t>(uy0) * m_mask_w + ux0;
                ctx->UpdateSubresource(m_mask_tex.Get(), 0, &box, src_ptr, m_mask_w * sizeof(uint32_t), 0);
            }
            m_mask_dirty = false;
            m_has_dirty_rect = false;
        }
    } else {
        m_mask_dirty = false;
        m_has_dirty_rect = false;
    }
}

void EraseTool::render_overlay(SharedControlBlock *b, ID3D11Device *device) {
    if (!m_active || !b || b->view_image_width == 0) return;
    ImGuiIO &io = ImGui::GetIO();
    if (device) {
        update_gpu_texture(device);
    }
    float w = (float)b->view_image_width, h = (float)b->view_image_height;
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    if (m_mask_srv && m_mask_white_px > 0) {
        draw->AddImageQuad((ImTextureID)m_mask_srv.Get(),
            image_to_screen(ImVec2(0, 0), b, io.DisplaySize), image_to_screen(ImVec2(w, 0), b, io.DisplaySize),
            image_to_screen(ImVec2(w, h), b, io.DisplaySize), image_to_screen(ImVec2(0, h), b, io.DisplaySize));
    }
    ImVec2 ip = screen_to_image(io.MousePos, b, io.DisplaySize);
    bool in_ui = (io.MousePos.x >= m_ui_min.x && io.MousePos.x <= m_ui_max.x &&
                  io.MousePos.y >= m_ui_min.y && io.MousePos.y <= m_ui_max.y) ||
                 ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
    if (!in_ui && !(b->view_interaction_flags & VIEW_FLAG_BEFORE_AFTER) && ip.x >= 0 && ip.x < w && ip.y >= 0 && ip.y < h) {
        bool is_sub = subtract_mode || io.MouseDown[1];
        draw->AddCircle(io.MousePos, (std::max)(2.0f, brush_radius), is_sub ? IM_COL32(80, 80, 80, 230) : IM_COL32(255, 65, 65, 230), 36, 2.0f);
        draw->AddCircle(io.MousePos, (std::max)(2.0f, brush_radius) + 1.0f, IM_COL32(0, 0, 0, 190), 36, 1.0f);
        draw->AddCircleFilled(io.MousePos, 1.5f, IM_COL32(255, 255, 255, 220));
    }

    if (is_processing() && m_ui_min.x == 0.0f && m_ui_min.y == 0.0f) {
        float prog = get_progress();
        std::string st = get_status();
        char buf[128];
        if (prog >= 0.0f) snprintf(buf, sizeof(buf), "%s (%.0f%%)", st.empty() ? "Erasing..." : st.c_str(), prog * 100.0f);
        else snprintf(buf, sizeof(buf), "%s", st.empty() ? "Erasing..." : st.c_str());
        ImVec2 txt_sz = ImGui::CalcTextSize(buf);
        float pad_x = 16.0f, pad_y = 8.0f;
        float cx = io.DisplaySize.x * 0.5f;
        float top_y = 28.0f;
        ImVec2 p_min(cx - txt_sz.x * 0.5f - pad_x, top_y);
        ImVec2 p_max(cx + txt_sz.x * 0.5f + pad_x, top_y + txt_sz.y + pad_y * 2.0f);
        draw->AddRectFilled(p_min, p_max, IM_COL32(20, 20, 24, 230), 8.0f);
        draw->AddRect(p_min, p_max, IM_COL32(80, 80, 90, 255), 8.0f, 0, 1.0f);
        draw->AddText(ImVec2(p_min.x + pad_x, p_min.y + pad_y), IM_COL32(255, 255, 255, 255), buf);
    }
}

bool EraseTool::export_mask_png(const std::wstring &out_path, uint32_t width, uint32_t height, size_t *out_white_px) {
    if (m_mask.empty() || m_mask_w != width || m_mask_h != height) return false;
    recalculate_white_pixels();
    if (out_white_px) *out_white_px = m_mask_white_px;
    if (m_mask_white_px == 0) return false;
    FILE *f = _wfopen(out_path.c_str(), L"wb"); if (!f) return false;
    int ret = stbi_write_png_to_func([](void *c, void *d, int sz) { fwrite(d, 1, sz, (FILE*)c); }, f, width, height, 1, m_mask.data(), width);
    fclose(f); return ret != 0;
}

void EraseTool::handle_frame(SharedControlBlock *block, ID3D11Device *device, const std::wstring &active_path, bool bg_hovered, bool any_active, bool fine) {
    (void)bg_hovered; (void)any_active; (void)fine;
    if (!block || block->export_state != ExportState::Idle) return;
    if (m_active) ipc_set_view_flag(&block->view_interaction_flags, VIEW_FLAG_ERASE_ACTIVE);
    else ipc_clear_view_flag(&block->view_interaction_flags, VIEW_FLAG_ERASE_ACTIVE);
    if (!m_active) return;

    bool stroke_finished = false;
    handle_input(block, is_processing(), &stroke_finished);
    render_overlay(block, device);

    bool currently_erasing = is_processing();
    if (m_was_erasing && !currently_erasing && get_last_error().empty()) clear_mask();
    m_was_erasing = currently_erasing;

    bool enter_pressed = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    if ((enter_pressed || (auto_apply && stroke_finished)) && has_mask_edits() && !currently_erasing && !active_path.empty() && is_model_present()) {
        wchar_t tmp[MAX_PATH] = {}; GetTempPathW(MAX_PATH, tmp);
        std::wstring mask_p = std::wstring(tmp) + L"shaderlab_erase_mask.png";
        if (export_mask_png(mask_p, block->view_image_width, block->view_image_height))
            trigger_erase(active_path, mask_p);
    }
}

void EraseTool::render_ui(SharedControlBlock *block, const std::wstring &active_path) {
    try {
        if (!m_daemon_ready.load() && !m_daemon_prewarming.load() && is_model_present()) {
            prewarm_worker();
        }
        ImGui::Spacing(); ImGui::PushTextWrapPos(0.0f);

        bool active = m_active;
        if (active) {
            ImVec4 tab_col = ImGui::GetStyleColorVec4(ImGuiCol_TabActive);
            ImVec4 tab_hov = ImGui::GetStyleColorVec4(ImGuiCol_TabHovered);
            ImVec4 btn_act = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);

            ImGui::PushStyleColor(ImGuiCol_Button, tab_col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tab_hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, btn_act);
        }

        if (ImGui::Button(active ? "Erase Mode: ACTIVE (E)" : "Enable Erase Mode (E)", ImVec2(-1, 32))) {
            set_active(!active);
        }

        if (active) {
            ImGui::PopStyleColor(3);
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::SliderFloat("Brush Radius", &brush_radius, 2.0f, 200.0f, "%.0f px");
        ImGui::TextDisabled("Left-click to paint, right-click to unmask. [ and ] to resize.");
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::Checkbox("Auto-apply", &auto_apply);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatically erase immediately after drawing.");
        ImGui::Spacing();

        bool is_erasing = is_processing(), has_mask = has_mask_edits();
        if (is_erasing) {
            ImGui::ProgressBar(get_progress(), ImVec2(-1, 28), get_status().empty() ? "Erasing object..." : get_status().c_str());
        } else if (is_downloading()) {
            float dp = get_download_progress();
            char ptxt[64];
            if (dp > 0.0f) snprintf(ptxt, sizeof(ptxt), "Downloading: %.0f%%", dp * 100.0f);
            else snprintf(ptxt, sizeof(ptxt), "Downloading LaMa Big HD (~196MB)...");
            ImGui::ProgressBar(dp > 0.0f ? dp : -1.0f, ImVec2(-1, 28), ptxt);
        } else if (!is_model_present()) {
            if (ImGui::Button("Download LaMa Big HD (196MB)", ImVec2(-1, 28))) {
                trigger_model_download();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Download the neural model (common/big-lama.pt) required for object erasing.");
        } else {
            ImGui::BeginDisabled(!has_mask || active_path.empty());
            if (ImGui::Button("Erase Object", ImVec2(-1, 28))) {
                wchar_t tmp[MAX_PATH] = {}; GetTempPathW(MAX_PATH, tmp);
                std::wstring mask_p = std::wstring(tmp) + L"shaderlab_erase_mask.png";
                if (block && export_mask_png(mask_p, block->view_image_width, block->view_image_height))
                    trigger_erase(active_path, mask_p);
            }
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        ImGui::BeginDisabled(!has_mask || is_erasing);
        if (ImGui::Button("Clear Mask", ImVec2(-1, 24))) clear_mask();
        ImGui::EndDisabled();

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        size_t total = get_total_erase_steps();
        if (total > 0) ImGui::Text("Step %u of %u", (uint32_t)get_current_erase_step(), (uint32_t)total);
        else ImGui::TextUnformatted("No erase edits yet.");

        bool has_depth = (block && block->depth_valid != 0);
        if (has_depth) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextUnformatted("Depth Buffer:");
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Deleted a large object? Re-generate the depth buffer.");
            ImGui::PopStyleColor();
            ImGui::Spacing();

            DepthManager &depth_mgr = DepthManager::get();
            bool depth_busy = depth_mgr.is_processing();
            if (depth_busy) {
                float dp = depth_mgr.get_progress();
                std::string st = depth_mgr.get_status();
                ImGui::ProgressBar(dp >= 0.0f ? dp : -1.0f, ImVec2(-1, 28), st.empty() ? "Generating depth..." : st.c_str());
            } else {
                if (ImGui::Button("Re-generate Depth Buffer", ImVec2(-1, 28))) {
                    depth_mgr.trigger_depth_estimate(active_path);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Re-computes depth for the current image.");
                }
            }
        }

        std::string err = get_last_error();
        if (!err.empty()) { ImGui::Spacing(); ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", err.c_str()); }
        ImGui::PopTextWrapPos();
    } catch (const std::exception &ex) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Erase error: %s", ex.what());
        ImGui::PopTextWrapPos();
    }
}

std::vector<fs::path> EraseTool::get_base_dirs() const {
    std::vector<fs::path> dirs; std::error_code ec;
    HMODULE hMod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&EraseTool::get, &hMod) && hMod) {
        wchar_t p[MAX_PATH] = {}; if (GetModuleFileNameW(hMod, p, MAX_PATH) > 0) dirs.push_back(fs::path(p).parent_path());
    }
    wchar_t ep[MAX_PATH] = {}; if (GetModuleFileNameW(nullptr, ep, MAX_PATH) > 0) dirs.push_back(fs::path(ep).parent_path());
    fs::path cwd = fs::current_path(ec); if (!ec && !cwd.empty()) dirs.push_back(cwd);
    return dirs;
}

static fs::path find_in_paths(const std::vector<fs::path> &bases, const std::vector<const wchar_t *> &rels, size_t min_sz = 1) {
    std::error_code ec;
    for (auto cur : bases) {
        for (int i = 0; i <= 5; ++i) {
            for (auto r : rels) {
                auto p = cur / r;
                if (fs::exists(p, ec) && fs::file_size(p, ec) >= min_sz) return fs::absolute(p, ec);
            }
            if (!cur.has_parent_path() || cur == cur.parent_path()) break;
            cur = cur.parent_path();
        }
    }
    return {};
}

std::wstring EraseTool::find_lama_script_path() const {
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    if (!m_cached_script.empty() && fs::exists(m_cached_script)) return m_cached_script;
    auto p = find_in_paths(get_base_dirs(), {
        L"tools/lama/run_lama.py", L"lama/run_lama.py", L"run_lama.py", L"tools/run_lama.py",
        L"test/tools/lama/run_lama.py", L"tools/migan/run_migan.py"
    }, 100);
    return m_cached_script = p.wstring();
}

std::wstring EraseTool::find_lama_model_path() const {
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    if (!m_cached_model.empty() && fs::exists(m_cached_model)) return m_cached_model;
    auto p = find_in_paths(get_base_dirs(), {
        L"common/big-lama.pt", L"tools/lama/big-lama.pt", L"big-lama.pt", L"test/common/big-lama.pt"
    }, 20000000);
    return m_cached_model = p.wstring();
}

std::wstring EraseTool::find_python_executable() const {
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    if (!m_cached_py.empty() && fs::exists(m_cached_py)) return m_cached_py;
    auto p = find_in_paths(get_base_dirs(), {
        L"tools/python/python.exe", L"python/python.exe", L"tools/depth_anything_v2/venv/Scripts/python.exe", L"venv/Scripts/python.exe", L"test/tools/python/python.exe"
    }, 1024);
    if (!p.empty()) return m_cached_py = p.wstring();
    for (auto cmd : { L"python.exe", L"py.exe", L"C:\\Python314\\python.exe", L"C:\\Python313\\python.exe", L"C:\\Python312\\python.exe", L"C:\\Python311\\python.exe" }) {
        wchar_t out[MAX_PATH];
        if (SearchPathW(nullptr, cmd, nullptr, MAX_PATH, out, nullptr) > 0 && fs::exists(out)) return m_cached_py = out;
        if (fs::exists(cmd)) return m_cached_py = cmd;
    }
    return L"";
}

bool EraseTool::is_model_present() const { return !find_lama_model_path().empty(); }

class EraseModelDownloadCallback : public IBindStatusCallback {
public:
    std::atomic<float> *progress_out = nullptr;
    std::atomic<bool> *cancel_flag = nullptr;
    STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *ppv = static_cast<IBindStatusCallback*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
    STDMETHOD(OnStartBinding)(DWORD, IBinding*) override { return S_OK; }
    STDMETHOD(GetPriority)(LONG*) override { return S_OK; }
    STDMETHOD(OnLowResource)(DWORD) override { return S_OK; }
    STDMETHOD(OnProgress)(ULONG ulProgress, ULONG ulProgressMax, ULONG, LPCWSTR) override {
        if (cancel_flag && cancel_flag->load()) return E_ABORT;
        if (progress_out && ulProgressMax > 0) {
            progress_out->store((float)ulProgress / (float)ulProgressMax);
        }
        return S_OK;
    }
    STDMETHOD(OnStopBinding)(HRESULT, LPCWSTR) override { return S_OK; }
    STDMETHOD(GetBindInfo)(DWORD*, BINDINFO*) override { return S_OK; }
    STDMETHOD(OnDataAvailable)(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    STDMETHOD(OnObjectAvailable)(REFIID, IUnknown*) override { return S_OK; }
};

bool EraseTool::trigger_model_download() {
    if (is_model_present()) return true;
    if (m_downloading.exchange(true)) return false;

    m_download_progress.store(0.0f);
    m_cancel_download.store(false);
    {
        std::lock_guard<std::mutex> l(m_status_mutex);
        m_download_status = "Downloading LaMa Big HD (~196MB)...";
    }

    if (m_download_thread.joinable()) {
        m_download_thread.join();
    }

    m_download_thread = std::thread([this]() {
        fs::path dest = get_base_dirs().empty() ? fs::current_path() / "common" : get_base_dirs()[0] / "common";
        fs::create_directories(dest);
        fs::path tmp = dest / "big-lama.pt.tmp", target = dest / "big-lama.pt";

        EraseModelDownloadCallback cb;
        cb.progress_out = &m_download_progress;
        cb.cancel_flag = &m_cancel_download;

        bool downloaded = false;
        for (auto u : { L"https://github.com/Sanster/models/releases/download/add_big_lama/big-lama.pt",
                        L"https://hf-mirror.com/fashn-ai/LaMa/resolve/main/big-lama.pt",
                        L"https://huggingface.co/fashn-ai/LaMa/resolve/main/big-lama.pt" }) {
            if (m_cancel_download.load()) break;
            m_download_progress.store(0.0f);
            if (SUCCEEDED(URLDownloadToFileW(nullptr, u, tmp.wstring().c_str(), 0, &cb)) &&
                fs::exists(tmp) && fs::file_size(tmp) >= 100000000) {
                std::error_code ec;
                fs::rename(tmp, target, ec);
                if (!ec && fs::exists(target)) {
                    downloaded = true;
                    break;
                }
            }
            std::error_code ec;
            fs::remove(tmp, ec);
        }

        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            m_cached_model.clear();
        }

        {
            std::lock_guard<std::mutex> l(m_status_mutex);
            if (downloaded) {
                m_download_status = "LaMa Big HD downloaded successfully!";
                log(3, "LaMa Big HD downloaded successfully to " + wide_to_utf8(target.wstring().c_str()));
            } else {
                m_download_status = "Failed to download LaMa Big HD";
                log(2, "Failed to download LaMa Big HD from all mirror sources");
            }
        }
        m_downloading.store(false);

        if (downloaded && !m_cancel_download.load()) {
            prewarm_worker();
        }
    });
    return true;
}

std::string EraseTool::get_download_status() const {
    std::lock_guard<std::mutex> l(m_status_mutex);
    return m_download_status;
}

std::string EraseTool::get_model_name() const {
    std::wstring p = find_lama_model_path();
    if (p.empty()) return "None";
    return "LaMa Big HD";
}

static bool run_cmd(const std::wstring &cmd, const std::wstring &dir, HANDLE in_h = nullptr, HANDLE out_h = nullptr, PROCESS_INFORMATION *out_pi = nullptr) {
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW | (in_h || out_h ? STARTF_USESTDHANDLES : 0);
    si.wShowWindow = SW_HIDE; si.hStdInput = in_h; si.hStdOutput = out_h; si.hStdError = out_h;
    PROCESS_INFORMATION pi = {}; std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, in_h || out_h ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
    if (!ok) return false;
    if (out_pi) *out_pi = pi;
    else {
        DWORD wait_res = WaitForSingleObject(pi.hProcess, 120000);
        DWORD code = 1;
        if (wait_res == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
        } else {
            GetExitCodeProcess(pi.hProcess, &code);
        }
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return code == 0 && wait_res == WAIT_OBJECT_0;
    }
    return true;
}

bool EraseTool::prewarm_worker() {
    bool needs_stop = false;
    {
        std::lock_guard<std::mutex> lock(m_daemon_mutex);
        if (m_daemon_pi.hProcess) {
            DWORD code = 0;
            if (GetExitCodeProcess(m_daemon_pi.hProcess, &code) && code == STILL_ACTIVE) return true;
            needs_stop = true;
        }
    }
    if (needs_stop) stop_worker();

    if (m_daemon_prewarming.load()) return true;
    std::wstring py = find_python_executable(), script = find_lama_script_path(), model = find_lama_model_path();
    if (py.empty() || script.empty() || model.empty()) return false;

    m_daemon_prewarming = true; m_daemon_ready = false; m_daemon_stopping = false;
    if (m_daemon_thread.joinable()) m_daemon_thread.join();

    m_daemon_thread = std::thread([this, py, script, model]() {
        HANDLE in_rd = nullptr, in_wr = nullptr, out_rd = nullptr, out_wr = nullptr;
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        if (!CreatePipe(&in_rd, &in_wr, &sa, 0) || !CreatePipe(&out_rd, &out_wr, &sa, 0)) { m_daemon_prewarming = false; return; }
        SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);

        std::wstring cmd = L"\"" + py + L"\" \"" + fs::absolute(script).wstring() + L"\" --daemon --model \"" + fs::absolute(model).wstring() + L"\"";
        PROCESS_INFORMATION pi = {};
        BOOL ok = run_cmd(cmd, fs::path(script).parent_path().wstring(), in_rd, out_wr, &pi);
        CloseHandle(in_rd); CloseHandle(out_wr);

        if (!ok) { CloseHandle(in_wr); CloseHandle(out_rd); m_daemon_prewarming = false; return; }
        {
            std::lock_guard<std::mutex> dlock(m_daemon_mutex);
            m_daemon_pi = pi; m_daemon_in = in_wr; m_daemon_out = out_rd;
        }

        char buf[512]; DWORD br = 0; std::string line;
        while (!m_daemon_stopping.load() && m_daemon_out && ReadFile(m_daemon_out, buf, sizeof(buf), &br, nullptr) && br > 0) {
            for (DWORD i = 0; i < br; ++i) {
                char ch = buf[i];
                if (ch == '\n' || ch == '\r') {
                    if (!line.empty()) {
                        if (line.find("[READY]") != std::string::npos) {
                            m_daemon_ready = true;
                            m_daemon_prewarming = false;
                            log(0, "[Daemon] " + line);
                        } else if (line.find("[DONE]") != std::string::npos) {
                            {
                                std::lock_guard<std::mutex> sl(m_status_mutex);
                                m_daemon_result_line = line;
                                m_daemon_has_result = true;
                            }
                            m_daemon_cv.notify_all();
                        } else if (line.find("[ERROR]") != std::string::npos) {
                            {
                                std::lock_guard<std::mutex> sl(m_status_mutex);
                                m_daemon_result_line = line;
                                m_daemon_has_result = true;
                                m_last_error = line;
                            }
                            m_daemon_cv.notify_all();
                        } else {
                            auto p = line.find("[PROGRESS]");
                            if (p != std::string::npos) {
                                m_progress.store((float)atoi(line.c_str() + p + 10) / 100.0f);
                                std::lock_guard<std::mutex> sl(m_status_mutex);
                                m_status = line.substr(p + 10);
                            } else {
                                log(0, "[Daemon] " + line);
                            }
                        }
                        line.clear();
                    }
                } else {
                    line += ch;
                }
            }
        }
        m_daemon_prewarming = false;
        m_daemon_ready = false;
        {
            std::lock_guard<std::mutex> sl(m_status_mutex);
            m_daemon_has_result = true;
        }
        m_daemon_cv.notify_all();
    });
    return true;
}

void EraseTool::stop_worker() {
    m_daemon_stopping = true;
    m_daemon_cv.notify_all();

    HANDLE proc = nullptr, th = nullptr, in_wr = nullptr, out_rd = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_daemon_mutex);
        m_daemon_ready = false; m_daemon_prewarming = false;
        in_wr = m_daemon_in; m_daemon_in = nullptr;
        out_rd = m_daemon_out; m_daemon_out = nullptr;
        proc = m_daemon_pi.hProcess; m_daemon_pi.hProcess = nullptr;
        th = m_daemon_pi.hThread; m_daemon_pi.hThread = nullptr;
    }
    if (in_wr) {
        const char *q = "{\"cmd\":\"quit\"}\n";
        DWORD bw = 0;
        WriteFile(in_wr, q, (DWORD)strlen(q), &bw, nullptr);
        CloseHandle(in_wr);
    }
    if (proc) {
        if (WaitForSingleObject(proc, 500) == WAIT_TIMEOUT) {
            TerminateProcess(proc, 0);
        }
        CloseHandle(proc);
    }
    if (th) CloseHandle(th);
    if (out_rd) CloseHandle(out_rd);

    if (m_daemon_thread.joinable() && m_daemon_thread.get_id() != std::this_thread::get_id()) {
        m_daemon_thread.join();
    }
    m_daemon_stopping = false;
}

static fs::path get_stage_cache_dir(const std::wstring &dir_key) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring key = dir_key.empty() ? L"default" : dir_key;
    return fs::path(tmp) / L"ShaderLab" / L"workspace" / L"erase_stages" / key;
}

static bool is_erase_stage_path(const std::wstring &path) {
    std::wstring p = path;
    for (auto &c : p) c = towlower(c);
    return p.find(L"erase_stages") != std::wstring::npos;
}

void EraseTool::sync_stage_to_ui(const std::wstring &dir_key, size_t idx, size_t total, const std::wstring &img_path, const std::wstring &sidecar_path) {
    if (m_stage_cb) {
        try {
            m_stage_cb(dir_key, idx, total, img_path, sidecar_path);
        } catch (...) {}
    }
    OutputDebugStringW((L"[Erase] Stage (" + (dir_key.empty() ? L"default" : dir_key) + L"): " + std::to_wstring(idx) + L"/" + std::to_wstring(total) + L" - " + img_path + L"\n").c_str());
}

bool EraseTool::ensure_stage_cache_initialized(const std::wstring &base_image_path) {
    std::lock_guard<std::mutex> l(m_stage_mutex);
    if (base_image_path.empty()) return false;
    std::error_code ec;
    fs::path base_p = fs::absolute(base_image_path, ec);
    if (ec || !fs::exists(base_p, ec)) return false;

    if (!m_stages.empty() && fs::exists(m_stages[0].img, ec)) {
        if (!m_base_img.empty() && fs::equivalent(fs::path(m_base_img), base_p, ec)) return true;
        for (const auto &st : m_stages) {
            std::error_code eq_ec;
            if (fs::equivalent(fs::path(st.img), base_p, eq_ec) && fs::exists(st.img, ec)) return true;
        }
        if (is_erase_stage_path(base_p.wstring())) return true;
    }

    fs::path dir = get_stage_cache_dir(m_stage_dir_key);
    fs::create_directories(dir, ec);
    for (const auto &e : fs::directory_iterator(dir, ec)) {
        std::error_code eq_ec;
        bool is_same = false;
        try {
            is_same = fs::equivalent(e.path(), base_p, eq_ec);
        } catch (...) {
            is_same = (e.path() == base_p);
        }
        if (!is_same) fs::remove_all(e.path(), ec);
    }

    fs::path s0_img = dir / (L"stage_0" + base_p.extension().wstring());
    if (base_p != s0_img) fs::copy_file(base_p, s0_img, fs::copy_options::overwrite_existing, ec);
    if (ec) { log(2, "Failed to copy stage 0: " + ec.message()); return false; }

    fs::path sc_p = base_p; sc_p.replace_extension(".sldepth");
    fs::path s0_sc = dir / L"stage_0.sldepth";
    if (fs::exists(sc_p, ec) && !ec && sc_p != s0_sc) fs::copy_file(sc_p, s0_sc, fs::copy_options::overwrite_existing, ec);

    m_base_img = base_p.wstring();
    m_stages = { { s0_img.wstring(), fs::exists(s0_sc, ec) ? s0_sc.wstring() : L"" } };
    m_stage_idx = 0;
    return true;
}

bool EraseTool::trigger_erase(const std::wstring &base_image_path, const std::wstring &mask_path) {
    if (m_processing.load()) { log(1, "Inpainting already in progress..."); return false; }
    if (mask_path.empty() || !fs::exists(mask_path)) {
        std::lock_guard<std::mutex> l(m_status_mutex); m_last_error = "Mask not found: " + wide_to_utf8(mask_path.c_str()); return false;
    }
    if (!ensure_stage_cache_initialized(base_image_path)) {
        std::lock_guard<std::mutex> l(m_status_mutex); m_last_error = "Failed to initialize stage cache"; return false;
    }
    if (m_worker.joinable()) m_worker.join();

    m_processing = true; m_progress.store(-1.0f);
    {
        std::lock_guard<std::mutex> l(m_status_mutex);
        m_status = "Inpainting with " + get_model_name() + "..."; m_last_error.clear();
    }

    std::wstring in_img, in_sc, out_img, out_sc, job_dir_key; size_t next_idx = 0; bool has_sc = false;
    {
        std::lock_guard<std::mutex> l(m_stage_mutex);
        if (m_stages.empty() || m_stage_idx >= m_stages.size()) {
            m_processing = false;
            std::lock_guard<std::mutex> sl(m_status_mutex);
            m_last_error = "Invalid stage state";
            return false;
        }
        job_dir_key = m_stage_dir_key.empty() ? L"default" : m_stage_dir_key;
        m_processing_dir_key = job_dir_key;
        in_img = m_stages[m_stage_idx].img; in_sc = m_stages[m_stage_idx].sidecar;
        has_sc = !in_sc.empty() && fs::exists(in_sc);
        next_idx = m_stage_idx + 1;
        fs::path dir = fs::path(in_img).parent_path();
        out_img = (dir / (L"stage_" + std::to_wstring(next_idx) + L".png")).wstring();
        out_sc = (dir / (L"stage_" + std::to_wstring(next_idx) + L".sldepth")).wstring();
        if (has_sc) { std::error_code ec; fs::copy_file(in_sc, out_sc, fs::copy_options::overwrite_existing, ec); }
    }

    m_worker = std::thread([this, in_img, out_img, out_sc, has_sc, mask_path, next_idx, job_dir_key]() {
        bool success = false;
        try {
            if (!m_daemon_ready.load() && m_daemon_prewarming.load()) {
                for (int i = 0; i < 200 && m_daemon_prewarming.load() && !m_daemon_ready.load(); ++i) Sleep(50);
            }

            auto esc = [](const std::string &s) {
                std::string o;
                for (char c : s) { if (c == '\\') o += "\\\\"; else if (c == '"') o += "\\\""; else o += c; }
                return o;
            };
            std::string req = "{\"cmd\":\"inpaint\",\"input\":\"" + esc(wide_to_utf8(in_img.c_str())) +
                              "\",\"mask\":\"" + esc(wide_to_utf8(mask_path.c_str())) +
                              "\",\"output\":\"" + esc(wide_to_utf8(out_img.c_str())) + "\"}\n";

            bool wrote = false;
            {
                std::lock_guard<std::mutex> l(m_daemon_mutex);
                if (!m_daemon_stopping.load() && m_daemon_ready.load() && m_daemon_in && m_daemon_out) {
                    DWORD code = 0;
                    if (GetExitCodeProcess(m_daemon_pi.hProcess, &code) && code == STILL_ACTIVE) {
                        {
                            std::lock_guard<std::mutex> sl(m_status_mutex);
                            m_daemon_result_line.clear();
                            m_daemon_has_result = false;
                        }
                        DWORD bw = 0;
                        wrote = (WriteFile(m_daemon_in, req.c_str(), (DWORD)req.size(), &bw, nullptr) != FALSE);
                    }
                }
            }

            if (wrote) {
                std::unique_lock<std::mutex> sl(m_status_mutex);
                bool wait_ok = m_daemon_cv.wait_for(sl, std::chrono::seconds(120), [this] {
                    return m_daemon_has_result || m_daemon_stopping.load();
                });
                if (wait_ok && m_daemon_result_line.find("[DONE]") != std::string::npos) {
                    success = true;
                }
            }

            if (!success) {
                std::wstring py = find_python_executable(), script = find_lama_script_path(), model = find_lama_model_path();
                if (!py.empty() && !script.empty() && !model.empty()) {
                    std::wstring cmd = L"\"" + py + L"\" \"" + fs::absolute(script).wstring() + L"\" --input \"" + in_img +
                                       L"\" --mask \"" + fs::absolute(mask_path).wstring() + L"\" --output \"" + out_img +
                                       L"\" --model \"" + fs::absolute(model).wstring() + L"\" --device auto";
                    log(0, "Fallback CLI: " + wide_to_utf8(cmd.c_str()));
                    if (run_cmd(cmd, fs::path(script).parent_path().wstring()) && fs::exists(out_img)) {
                        success = true;
                        std::lock_guard<std::mutex> sl(m_status_mutex);
                        m_last_error.clear();
                    }
                }
            }

            // Verify output image file on disk and wait briefly for full file flush
            if (success) {
                bool file_ready = false;
                for (int attempt = 0; attempt < 50; ++attempt) {
                    std::error_code ec;
                    auto sz = fs::file_size(out_img, ec);
                    if (!ec && sz > 0) {
                        // Attempt to open read-only in binary mode to ensure no sharing/write locks remain
                        FILE *tf = nullptr;
                        if (_wfopen_s(&tf, out_img.c_str(), L"rb") == 0 && tf) {
                            fclose(tf);
                            file_ready = true;
                            break;
                        }
                    }
                    Sleep(20);
                }
                if (!file_ready) {
                    success = false;
                    std::lock_guard<std::mutex> sl(m_status_mutex);
                    m_last_error = "Inpaint output file was empty or locked on disk";
                    log(2, m_last_error);
                }
            }

            if (success) {
                m_progress.store(1.0f);
                { std::lock_guard<std::mutex> l(m_status_mutex); m_status = "Erase complete!"; }
                std::wstring final_sc = has_sc ? out_sc : L"";
                size_t total_steps = next_idx;
                {
                    std::lock_guard<std::mutex> l(m_stage_mutex);
                    std::wstring cur_key = m_stage_dir_key.empty() ? L"default" : m_stage_dir_key;
                    if (cur_key == job_dir_key) {
                        m_stages.resize(next_idx + 1);
                        m_stages[next_idx] = { out_img, final_sc };
                        m_stage_idx = next_idx;
                        total_steps = m_stages.size() - 1;
                    }
                    m_processing_dir_key.clear();
                }
                sync_stage_to_ui(job_dir_key, next_idx, total_steps, out_img, final_sc);
                log(3, "Erase completed successfully: stage " + std::to_string(next_idx));
            } else {
                {
                    std::lock_guard<std::mutex> l(m_stage_mutex);
                    m_processing_dir_key.clear();
                }
                std::lock_guard<std::mutex> l(m_status_mutex);
                if (m_last_error.empty()) m_last_error = "Inpainting failed or timed out";
                log(2, m_last_error);
            }
        } catch (const std::exception &e) {
            {
                std::lock_guard<std::mutex> l(m_stage_mutex);
                m_processing_dir_key.clear();
            }
            std::lock_guard<std::mutex> l(m_status_mutex);
            m_last_error = std::string("Error: ") + e.what(); log(2, m_last_error);
        }
        m_processing.store(false);
    });
    return true;
}

bool EraseTool::set_stage_index(size_t target_idx) {
    if (m_processing.load()) return false;
    std::wstring key, img, sc;
    size_t total = 0;
    {
        std::lock_guard<std::mutex> l(m_stage_mutex);
        if (m_stages.empty() || target_idx >= m_stages.size()) return false;
        std::error_code ec;
        if (!fs::exists(m_stages[target_idx].img, ec) || ec) return false;
        m_stage_idx = target_idx;
        key = m_stage_dir_key.empty() ? L"default" : m_stage_dir_key;
        total = m_stages.size() - 1;
        img = m_stages[m_stage_idx].img;
        sc = m_stages[m_stage_idx].sidecar;
    }
    sync_stage_to_ui(key, target_idx, total, img, sc);
    return true;
}

bool EraseTool::undo_erase() {
    if (m_processing.load()) return false;
    std::wstring key, img, sc;
    size_t target = 0, total = 0;
    {
        std::lock_guard<std::mutex> l(m_stage_mutex);
        if (m_stage_idx == 0 || m_stages.empty()) return false;
        target = m_stage_idx - 1;
        std::error_code ec;
        if (!fs::exists(m_stages[target].img, ec) || ec) return false;
        m_stage_idx = target;
        key = m_stage_dir_key.empty() ? L"default" : m_stage_dir_key;
        total = m_stages.size() - 1;
        img = m_stages[m_stage_idx].img;
        sc = m_stages[m_stage_idx].sidecar;
    }
    sync_stage_to_ui(key, target, total, img, sc);
    return true;
}

bool EraseTool::redo_erase() {
    if (m_processing.load()) return false;
    std::wstring key, img, sc;
    size_t target = 0, total = 0;
    {
        std::lock_guard<std::mutex> l(m_stage_mutex);
        if (m_stages.empty() || m_stage_idx + 1 >= m_stages.size()) return false;
        target = m_stage_idx + 1;
        std::error_code ec;
        if (!fs::exists(m_stages[target].img, ec) || ec) return false;
        m_stage_idx = target;
        key = m_stage_dir_key.empty() ? L"default" : m_stage_dir_key;
        total = m_stages.size() - 1;
        img = m_stages[m_stage_idx].img;
        sc = m_stages[m_stage_idx].sidecar;
    }
    sync_stage_to_ui(key, target, total, img, sc);
    return true;
}

std::wstring EraseTool::get_current_stage_path() const {
    std::lock_guard<std::mutex> l(m_stage_mutex);
    if (m_stages.empty() || m_stage_idx >= m_stages.size()) return L"";
    return m_stages[m_stage_idx].img;
}

std::wstring EraseTool::get_base_image_path() const {
    std::lock_guard<std::mutex> l(m_stage_mutex);
    return m_base_img;
}

void EraseTool::clear_erase_history() {
    clear_mask();
    std::wstring key_to_clean;
    {
        std::lock_guard<std::mutex> l(m_stage_mutex);
        key_to_clean = m_stage_dir_key;
        m_stages.clear(); m_stage_idx = 0; m_base_img.clear();
    }
    {
        std::lock_guard<std::mutex> sl(m_status_mutex);
        m_last_error.clear();
    }
    remove_stage_cache_dir(key_to_clean);
}

void EraseTool::set_stage_dir_key(const std::wstring &key) {
    std::lock_guard<std::mutex> l(m_stage_mutex);
    m_stage_dir_key = key;
}

void EraseTool::save_document_state(DocumentState &out_state) const {
    out_state.mask = m_mask;
    out_state.mask_w = m_mask_w;
    out_state.mask_h = m_mask_h;
    out_state.mask_white_px = m_mask_white_px;
    out_state.undo_stack = m_undo_stack;
    out_state.redo_stack = m_redo_stack;
    {
        std::lock_guard<std::mutex> l(m_stage_mutex);
        out_state.stage_dir_key = m_stage_dir_key;
        out_state.base_img = m_base_img;
        out_state.stages = m_stages;
        out_state.stage_idx = m_stage_idx;
    }
    {
        std::lock_guard<std::mutex> sl(m_status_mutex);
        out_state.last_error = m_last_error;
    }
}

void EraseTool::restore_document_state(const DocumentState &state, ID3D11Device *dev) {
    m_is_drawing = false;
    m_last_sub = false;
    m_last_pos = ImVec2(-1.0f, -1.0f);
    m_mask = state.mask;
    m_mask_w = state.mask_w;
    m_mask_h = state.mask_h;
    m_mask_white_px = state.mask_white_px;
    m_undo_stack = state.undo_stack;
    m_redo_stack = state.redo_stack;
    {
        std::lock_guard<std::mutex> l(m_stage_mutex);
        m_stage_dir_key = state.stage_dir_key;
        m_base_img = state.base_img;
        m_stages = state.stages;
        m_stage_idx = state.stage_idx;
    }
    {
        std::lock_guard<std::mutex> sl(m_status_mutex);
        m_last_error = state.last_error;
    }
    if (m_mask.empty() || m_mask_w == 0 || m_mask_h == 0) {
        m_mask_tex.Reset();
        m_mask_srv.Reset();
        m_gpu_rgba.clear();
        m_tex_w = 0;
        m_tex_h = 0;
        m_mask_dirty = false;
        m_has_dirty_rect = false;
    } else {
        recalculate_white_pixels();
        mark_dirty_all();
        if (dev) {
            update_gpu_texture(dev);
        }
    }
}

void EraseTool::remove_stage_cache_dir(const std::wstring &dir_key) {
    fs::path dir = get_stage_cache_dir(dir_key);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

bool EraseTool::copy_stage_cache_dir(const std::wstring &src_dir_key, const std::wstring &dst_dir_key, DocumentState &io_state) {
    fs::path src_dir = get_stage_cache_dir(src_dir_key);
    fs::path dst_dir = get_stage_cache_dir(dst_dir_key);
    std::error_code ec;
    fs::remove_all(dst_dir, ec);
    io_state.stage_dir_key = dst_dir_key;
    if (!fs::exists(src_dir, ec)) {
        io_state.stages.clear();
        io_state.stage_idx = 0;
        return true;
    }
    fs::create_directories(dst_dir, ec);
    fs::copy(src_dir, dst_dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    for (auto &st : io_state.stages) {
        if (!st.img.empty()) st.img = (dst_dir / fs::path(st.img).filename()).wstring();
        if (!st.sidecar.empty()) st.sidecar = (dst_dir / fs::path(st.sidecar).filename()).wstring();
    }
    if (!io_state.base_img.empty() && is_erase_stage_path(io_state.base_img)) {
        io_state.base_img = (dst_dir / fs::path(io_state.base_img).filename()).wstring();
    }
    return true;
}

void EraseTool::log(int severity, const std::string &msg) {
    try {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::stringstream ss;
        struct tm buf;
        localtime_s(&buf, &in_time_t);
        ss << std::put_time(&buf, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
        std::string ts = ss.str();

        const char *s = (severity == 1) ? "WARN" : (severity == 2) ? "ERROR" : (severity == 3) ? "SUCCESS" : "INFO";
        std::string line = "[" + ts + "] [" + s + "] [Erase] " + msg;

        std::cout << line << std::endl;
        OutputDebugStringA((line + "\n").c_str());

        wchar_t exe_path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
            fs::path log_file = fs::path(exe_path).parent_path() / "ShaderLabErase.log";
            std::ofstream ofs(log_file, std::ios::out | std::ios::app);
            if (ofs.is_open()) {
                ofs << line << "\n";
            }
        }
    } catch (...) {}
}
