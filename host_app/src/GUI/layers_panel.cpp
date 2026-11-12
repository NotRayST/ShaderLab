#include "layers_panel.h"
#include "gui_manager.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace gui {
namespace {

std::string truncate_layer_name(const std::string &text, float max_width) {
    if (text.empty() || max_width <= 8.0f) return text;
    if (ImGui::CalcTextSize(text.c_str()).x <= max_width) {
        return text;
    }
    const char *ellipsis = "...";
    float ell_w = ImGui::CalcTextSize(ellipsis).x;
    std::string out = text;
    while (!out.empty() && ImGui::CalcTextSize(out.c_str()).x + ell_w > max_width) {
        out.pop_back();
    }
    return out + ellipsis;
}

void draw_eye_icon(ImDrawList *dl, const ImVec2 &bmin, const ImVec2 &bmax, bool visible, bool hovered) {
    ImVec2 center((bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f);
    ImU32 col = visible
        ? (hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 224, 232, 255))
        : (hovered ? IM_COL32(150, 150, 160, 220) : IM_COL32(105, 108, 118, 200));

    if (visible) {
        dl->AddBezierCubic(
            ImVec2(center.x - 8.0f, center.y),
            ImVec2(center.x - 4.0f, center.y - 5.5f),
            ImVec2(center.x + 4.0f, center.y - 5.5f),
            ImVec2(center.x + 8.0f, center.y),
            col, 1.4f
        );
        dl->AddBezierCubic(
            ImVec2(center.x - 8.0f, center.y),
            ImVec2(center.x - 4.0f, center.y + 5.5f),
            ImVec2(center.x + 4.0f, center.y + 5.5f),
            ImVec2(center.x + 8.0f, center.y),
            col, 1.4f
        );
        dl->AddCircleFilled(center, 2.5f, col);
    } else {
        dl->AddLine(ImVec2(center.x - 6.0f, center.y + 4.0f), ImVec2(center.x + 6.0f, center.y - 4.0f), col, 1.3f);
        dl->AddBezierCubic(
            ImVec2(center.x - 7.0f, center.y),
            ImVec2(center.x - 3.0f, center.y + 3.0f),
            ImVec2(center.x + 3.0f, center.y + 3.0f),
            ImVec2(center.x + 7.0f, center.y),
            col, 1.2f
        );
    }
}

void draw_layer_thumbnail(ImDrawList *dl, const ImVec2 &tmin, const ImVec2 &tmax, const DocumentLayer &layer) {
    // Mini 4px alpha checkerboard behind layer thumbnail
    constexpr float kTile = 4.0f;
    dl->PushClipRect(tmin, tmax, true);
    int cols = static_cast<int>(std::ceil((tmax.x - tmin.x) / kTile));
    int rows = static_cast<int>(std::ceil((tmax.y - tmin.y) / kTile));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            ImU32 tile_col = ((r + c) & 1) ? IM_COL32(196, 196, 196, 255) : IM_COL32(245, 245, 245, 255);
            ImVec2 p0(tmin.x + c * kTile, tmin.y + r * kTile);
            ImVec2 p1(p0.x + kTile, p0.y + kTile);
            dl->AddRectFilled(p0, p1, tile_col);
        }
    }

    if (layer.srv && layer.width > 0 && layer.height > 0) {
        float box_w = tmax.x - tmin.x;
        float box_h = tmax.y - tmin.y;
        float aspect = static_cast<float>(layer.width) / static_cast<float>(layer.height);
        float draw_w = box_w;
        float draw_h = box_h;
        if (aspect > 1.0f) {
            draw_h = box_w / aspect;
        } else {
            draw_w = box_h * aspect;
        }
        float dx = (box_w - draw_w) * 0.5f;
        float dy = (box_h - draw_h) * 0.5f;
        dl->AddImage(
            reinterpret_cast<ImTextureID>(layer.srv.Get()),
            ImVec2(tmin.x + dx, tmin.y + dy),
            ImVec2(tmin.x + dx + draw_w, tmin.y + dy + draw_h)
        );
    }
    dl->PopClipRect();
    dl->AddRect(tmin, tmax, IM_COL32(85, 90, 100, 255), 1.0f);
}

bool should_draw_blend_separator(int mode_idx) {
    return mode_idx == 2 || mode_idx == 7 || mode_idx == 12 || mode_idx == 19;
}

} // namespace

bool render_layers_panel(
    GuiManager &gui,
    LayerStack &stack,
    LayerCompositor &compositor,
    ID3D11Device *device,
    ID3D11DeviceContext *context,
    const LayerPanelCallbacks *callbacks
) {
    bool changed = false;
    static uint32_t s_renaming_id = 0;
    static char s_rename_buf[128] = {};
    static bool s_rename_focus_next = false;

    const ImGuiStyle &style = ImGui::GetStyle();

    if (!ImGui::BeginChild("##LayersPanel", ImVec2(-1.0f, -1.0f), false)) {
        ImGui::EndChild();
        gui.register_dom_last_item("layers_panel", "Layers", "panel");
        return false;
    }

    DocumentLayer *active = stack.active_layer();
    bool has_active = (active != nullptr);

    // Header row: title + Up / Down reorder buttons
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.92f, 0.94f, 0.98f, 1.0f), "Layers");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", stack.layers.size());

    ImGui::SameLine();
    float right_edge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    float reorder_btn_w = 44.0f;
    float reorder_start_x = (std::max)(ImGui::GetCursorPosX(), right_edge - (reorder_btn_w * 2.0f + style.ItemSpacing.x));
    ImGui::SetCursorPosX(reorder_start_x);

    bool can_up = has_active && (stack.active_index < static_cast<int>(stack.layers.size()) - 1);
    bool can_down = has_active && (stack.active_index > 0);

    ImGui::BeginDisabled(!can_up);
    if (ImGui::Button("Up##layer_up", ImVec2(reorder_btn_w, 0.0f))) {
        if (stack.move_layer_up(stack.active_index)) changed = true;
    }
    ImGui::EndDisabled();
    gui.register_dom_last_item("layers_btn_up", "Move Layer Up", "button", false, can_up);

    ImGui::SameLine();
    ImGui::BeginDisabled(!can_down);
    if (ImGui::Button("Down##layer_dn", ImVec2(reorder_btn_w, 0.0f))) {
        if (stack.move_layer_down(stack.active_index)) changed = true;
    }
    ImGui::EndDisabled();
    gui.register_dom_last_item("layers_btn_down", "Move Layer Down", "button", false, can_down);

    // Blend Mode + Opacity row
    float avail_w = ImGui::GetContentRegionAvail().x;
    ImGui::BeginDisabled(!has_active);
    int cur_blend = has_active ? static_cast<int>(active->blend_mode) : 0;
    const char *cur_blend_label = blend_mode_name(static_cast<BlendMode>(cur_blend));
    ImGui::SetNextItemWidth((std::max)(100.0f, avail_w * 0.56f));
    if (ImGui::BeginCombo("##layers_blend_combo", cur_blend_label)) {
        for (int m = 0; m < static_cast<int>(BlendMode::Count); ++m) {
            if (should_draw_blend_separator(m)) {
                ImGui::Separator();
            }
            bool is_sel = (cur_blend == m);
            const char *mname = blend_mode_name(static_cast<BlendMode>(m));
            if (ImGui::Selectable(mname, is_sel)) {
                if (active && static_cast<int>(active->blend_mode) != m) {
                    active->blend_mode = static_cast<BlendMode>(m);
                    changed = true;
                }
            }
            if (is_sel) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    gui.register_dom_last_item("layers_combo_blend", cur_blend_label, "combo", false, has_active);

    ImGui::SameLine();
    float opacity_pct = has_active ? (active->opacity * 100.0f) : 100.0f;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##layers_opacity", &opacity_pct, 0.0f, 100.0f, "Op: %.0f%%")) {
        if (active) {
            active->opacity = std::clamp(opacity_pct / 100.0f, 0.0f, 1.0f);
            changed = true;
        }
    }
    gui.register_dom_last_item("layers_slider_opacity", "Layer Opacity", "slider", false, has_active);

    // Fill Opacity + Lock toggles row
    float fill_pct = has_active ? (active->fill_opacity * 100.0f) : 100.0f;
    ImGui::SetNextItemWidth((std::max)(85.0f, avail_w * 0.42f));
    if (ImGui::SliderFloat("##layers_fill", &fill_pct, 0.0f, 100.0f, "Fill: %.0f%%")) {
        if (active) {
            active->fill_opacity = std::clamp(fill_pct / 100.0f, 0.0f, 1.0f);
            changed = true;
        }
    }
    gui.register_dom_last_item("layers_slider_fill", "Layer Fill Opacity", "slider", false, has_active);

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Lock:");

    auto draw_lock_toggle = [&](const char *btn_label, const char *dom_id, const char *tooltip, bool active_flag, auto on_toggle) {
        ImGui::SameLine(0.0f, 3.0f);
        if (active_flag) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.48f, 0.80f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.56f, 0.88f, 1.0f));
        }
        if (ImGui::SmallButton(btn_label)) {
            on_toggle();
            changed = true;
        }
        if (active_flag) {
            ImGui::PopStyleColor(2);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", tooltip);
        }
        gui.register_dom_last_item(dom_id, tooltip, "checkbox", active_flag, has_active);
    };

    bool lock_a = has_active && active->lock_alpha;
    bool lock_p = has_active && active->lock_pixels;
    bool lock_m = has_active && active->lock_position;
    bool lock_all = has_active && active->is_locked_all();

    draw_lock_toggle("A##lk_a", "layers_lock_alpha", "Lock Transparent Pixels", lock_a, [&]() {
        if (active) active->lock_alpha = !active->lock_alpha;
    });
    draw_lock_toggle("P##lk_p", "layers_lock_pixels", "Lock Image Pixels", lock_p, [&]() {
        if (active) active->lock_pixels = !active->lock_pixels;
    });
    draw_lock_toggle("M##lk_m", "layers_lock_pos", "Lock Position", lock_m, [&]() {
        if (active) active->lock_position = !active->lock_position;
    });
    draw_lock_toggle("All##lk_all", "layers_lock_all", "Lock All", lock_all, [&]() {
        if (active) active->set_locked_all(!active->is_locked_all());
    });

    ImGui::EndDisabled();

    // Scrollable layer list (stored bottom-to-top, displayed top-to-bottom like Photoshop)
    float bottom_bar_h = ImGui::GetFrameHeight() + style.ItemSpacing.y * 2.0f;
    ImGui::BeginChild("##LayerListScroll", ImVec2(-1.0f, -bottom_bar_h), true);

    if (stack.layers.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("No layers in document. Click + New or Import below.");
        ImGui::PopTextWrapPos();
    } else {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        int count = static_cast<int>(stack.layers.size());
        for (int i = count - 1; i >= 0; --i) {
            DocumentLayer &layer = stack.layers[static_cast<size_t>(i)];
            bool is_selected = (i == stack.active_index);

            ImGui::PushID(static_cast<int>(layer.id));

            float row_w = ImGui::GetContentRegionAvail().x;
            constexpr float kRowHeight = 38.0f;
            ImVec2 row_start = ImGui::GetCursorScreenPos();
            ImVec2 row_end(row_start.x + row_w, row_start.y + kRowHeight);

            if (is_selected) {
                dl->AddRectFilled(row_start, row_end, IM_COL32(50, 92, 148, 120), 3.0f);
                dl->AddRect(row_start, row_end, IM_COL32(84, 142, 214, 180), 3.0f);
            } else {
                dl->AddRectFilled(row_start, row_end, IM_COL32(36, 38, 43, 140), 3.0f);
            }

            // Visibility Eye icon button (layer_vis_<i>)
            ImGui::SetCursorScreenPos(ImVec2(row_start.x + 4.0f, row_start.y + 3.0f));
            if (ImGui::InvisibleButton("##vis_btn", ImVec2(24.0f, 32.0f))) {
                layer.visible = !layer.visible;
                changed = true;
            }
            bool vis_hov = ImGui::IsItemHovered();
            draw_eye_icon(dl, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), layer.visible, vis_hov);
            if (vis_hov) {
                ImGui::SetTooltip("%s", layer.visible ? "Hide Layer" : "Show Layer");
            }
            gui.register_dom_last_item("layer_vis_" + std::to_string(i), layer.visible ? "Hide Layer" : "Show Layer", "checkbox", layer.visible);

            // 32x32 Thumbnail preview
            ImVec2 thumb_min(row_start.x + 32.0f, row_start.y + 3.0f);
            ImVec2 thumb_max(thumb_min.x + 32.0f, thumb_min.y + 32.0f);
            draw_layer_thumbnail(dl, thumb_min, thumb_max, layer);

            // Type badge + Layer name / Inline rename
            float text_x = thumb_max.x + 8.0f;
            float text_area_w = (std::max)(40.0f, row_end.x - text_x - 6.0f);
            const char *badge = layer.is_text_layer ? "[T]" : (layer.is_adjustment_layer ? "[Adj]" : "[Img]");

            if (s_renaming_id == layer.id) {
                ImGui::SetCursorScreenPos(ImVec2(text_x, row_start.y + 7.0f));
                ImGui::SetNextItemWidth(text_area_w);
                if (s_rename_focus_next) {
                    ImGui::SetKeyboardFocusHere();
                    s_rename_focus_next = false;
                }
                bool enter_pressed = ImGui::InputText(
                    "##rename_input",
                    s_rename_buf,
                    sizeof(s_rename_buf),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
                );
                gui.register_dom_last_item("layer_name_" + std::to_string(i), layer.name, "input", true);
                if (enter_pressed || ImGui::IsItemDeactivatedAfterEdit()) {
                    if (s_rename_buf[0] != '\0') {
                        layer.name = s_rename_buf;
                        changed = true;
                    }
                    s_renaming_id = 0;
                } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || !ImGui::IsItemActive()) {
                    if (!s_rename_focus_next && !ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        s_renaming_id = 0;
                    }
                }
            } else {
                ImGui::SetCursorScreenPos(ImVec2(text_x, row_start.y + 2.0f));
                if (ImGui::Selectable("##row_select", is_selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(text_area_w, kRowHeight - 4.0f))) {
                    stack.active_index = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (layer.is_text_layer && callbacks && callbacks->on_select_text_layer) {
                            callbacks->on_select_text_layer(i);
                        } else {
                            s_renaming_id = layer.id;
                            std::snprintf(s_rename_buf, sizeof(s_rename_buf), "%s", layer.name.c_str());
                            s_rename_focus_next = true;
                        }
                    }
                }
                if (ImGui::IsItemHovered() && !is_selected) {
                    dl->AddRect(row_start, row_end, IM_COL32(120, 140, 170, 120), 3.0f);
                }
                gui.register_dom_last_item("layer_row_" + std::to_string(i), layer.name, "list_item", is_selected);
                gui.register_dom_last_item("layer_name_" + std::to_string(i), layer.name, "text", is_selected);

                // Drag-and-drop reorder source & target
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
                    ImGui::SetDragDropPayload("SHADERLAB_LAYER_IDX", &i, sizeof(int));
                    ImGui::Text("%s %s", badge, layer.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("SHADERLAB_LAYER_IDX")) {
                        if (payload->DataSize == sizeof(int)) {
                            int from_idx = *static_cast<const int *>(payload->Data);
                            if (stack.reorder_layer(from_idx, i)) {
                                changed = true;
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                // Draw badge, truncated title, and subtitle inside row bounds
                dl->PushClipRect(ImVec2(text_x, row_start.y), ImVec2(row_end.x - 4.0f, row_end.y), true);
                ImU32 badge_col = layer.is_text_layer
                    ? IM_COL32(120, 195, 255, 255)
                    : (layer.is_adjustment_layer ? IM_COL32(255, 196, 95, 255) : IM_COL32(165, 175, 190, 255));
                float badge_w = ImGui::CalcTextSize(badge).x;
                dl->AddText(ImVec2(text_x, row_start.y + 4.0f), badge_col, badge);

                std::string disp_name = truncate_layer_name(layer.name, text_area_w - badge_w - 8.0f);
                ImU32 name_col = layer.visible ? IM_COL32(236, 238, 242, 255) : IM_COL32(135, 138, 145, 255);
                dl->AddText(ImVec2(text_x + badge_w + 5.0f, row_start.y + 4.0f), name_col, disp_name.c_str());

                char sub_buf[96] = {};
                std::snprintf(
                    sub_buf,
                    sizeof(sub_buf),
                    "%s | %.0f%%%s",
                    blend_mode_name(layer.blend_mode),
                    layer.opacity * 100.0f,
                    (layer.lock_alpha || layer.lock_pixels || layer.lock_position) ? " [Locked]" : ""
                );
                dl->AddText(ImVec2(text_x, row_start.y + 20.0f), IM_COL32(150, 156, 168, 220), sub_buf);
                dl->PopClipRect();
            }

            ImGui::SetCursorScreenPos(ImVec2(row_start.x, row_end.y + 3.0f));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // Bottom action toolbar (5 buttons fitted dynamically to panel width)
    float toolbar_w = ImGui::GetContentRegionAvail().x;
    float btn_w = (std::max)(36.0f, (toolbar_w - style.ItemSpacing.x * 4.0f) / 5.0f);

    if (ImGui::Button("+ New##lbtn_new", ImVec2(btn_w, 0.0f))) {
        if (stack.add_empty_layer("", device) >= 0) {
            changed = true;
        }
    }
    gui.register_dom_last_item("layers_btn_new", "New Layer", "button");

    ImGui::SameLine();
    if (ImGui::Button("Import##lbtn_imp", ImVec2(btn_w, 0.0f))) {
        if (callbacks && callbacks->on_import_clicked) {
            callbacks->on_import_clicked();
        } else {
            HWND owner = callbacks ? callbacks->owner_hwnd : nullptr;
            std::wstring chosen;
            const wchar_t *filter = L"Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.webp\0All Files\0*.*\0";
            if (open_file_dialog(owner, filter, chosen)) {
                if (stack.import_image_layer(chosen, device) >= 0) {
                    changed = true;
                }
            }
        }
    }
    gui.register_dom_last_item("layers_btn_import", "Import Image Layer", "button");

    ImGui::SameLine();
    ImGui::BeginDisabled(!has_active);
    if (ImGui::Button("Dup##lbtn_dup", ImVec2(btn_w, 0.0f))) {
        if (stack.duplicate_active_layer(device) >= 0) {
            changed = true;
        }
    }
    ImGui::EndDisabled();
    gui.register_dom_last_item("layers_btn_dup", "Duplicate Layer", "button", false, has_active);

    bool can_merge = has_active && (stack.active_index > 0) &&
                     !stack.layers[static_cast<size_t>(stack.active_index - 1)].lock_pixels;
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_merge);
    if (ImGui::Button("Merge##lbtn_merge", ImVec2(btn_w, 0.0f))) {
        if (stack.merge_down(device, context, compositor)) {
            changed = true;
        }
    }
    ImGui::EndDisabled();
    gui.register_dom_last_item("layers_btn_merge", "Merge Down", "button", false, can_merge);

    bool can_del = has_active && (!active->lock_pixels || stack.layers.size() > 1);
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_del);
    if (ImGui::Button("Del##lbtn_del", ImVec2(btn_w, 0.0f))) {
        if (stack.delete_active_layer(device)) {
            changed = true;
        }
    }
    ImGui::EndDisabled();
    gui.register_dom_last_item("layers_btn_del", "Delete Layer", "button", false, can_del);

    ImGui::EndChild();
    gui.register_dom_last_item("layers_panel", "Layers", "panel");

    if (changed && callbacks && callbacks->on_layers_changed) {
        callbacks->on_layers_changed();
    }
    return changed;
}

} // namespace gui
