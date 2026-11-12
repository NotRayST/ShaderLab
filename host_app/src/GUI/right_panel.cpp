#include "right_panel.h"
#include "gui_manager.h"
#include "tool_requirements.h"
#include "../erase_tool.h"
#include "../depth_manager.h"
#include <imgui.h>
#include <algorithm>

namespace gui {

void render_right_panel(GuiManager &gui, float right_offset, float top_offset, float panel_width, float panel_height) {
    ImGui::SetNextWindowPos(ImVec2(right_offset, top_offset));
    ImGui::SetNextWindowSize(ImVec2(panel_width, panel_height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
    ImGui::Begin("##RightParametersPanel", nullptr, flags);

    float avail_width = ImGui::GetContentRegionAvail().x;
    float item_spacing_x = ImGui::GetStyle().ItemSpacing.x;
    float tab_w = (avail_width - 2.0f * item_spacing_x) / 3.0f;
    float tab_h = 24.0f;

    auto render_segment_tab = [&](const char *label, int tab_idx, const char *dom_id, const char *alt_dom_id = nullptr) {
        bool is_active = (gui.m_right_panel_tab == tab_idx);
        if (is_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.23f, 0.40f, 0.65f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.48f, 0.75f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.17f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.22f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.72f, 1.0f));
        }

        std::string btn_id = std::string(label) + "##seg_tab";
        if (ImGui::Button(btn_id.c_str(), ImVec2(tab_w, tab_h))) {
            gui.select_right_panel_tab(tab_idx);
        }
        gui.register_dom_last_item(dom_id, label, "tab", is_active);
        if (alt_dom_id) {
            gui.register_dom_last_item(alt_dom_id, label, "tab", is_active);
        }

        ImGui::PopStyleColor(3);
    };

    render_segment_tab("Shaders", 0, "tab_shaders");
    ImGui::SameLine();
    render_segment_tab("3D Depth", 1, "tab_depth", "accordion_depth");
    ImGui::SameLine();
    render_segment_tab("Erase", 2, "tab_erase", "accordion_erase");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Scrollable properties region leaving room for pinned bottom button
    float bottom_btn_h = 24.0f;
    float spacing_y = ImGui::GetStyle().ItemSpacing.y;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.13f, 0.6f));
    ImGui::BeginChild("##PropertiesScrollArea", ImVec2(-1.0f, -(bottom_btn_h + spacing_y)), true);

    if (gui.m_right_panel_tab == 0) {
        // --- TAB 0: SHADERS / EFFECT UNIFORMS ---
        if (gui.m_selected_effect_idx >= 0 && gui.m_selected_effect_idx < static_cast<int>(gui.m_effects.size())) {
            auto &fx = gui.m_effects[gui.m_selected_effect_idx];

            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "%s", fx.name.c_str());
            ImGui::PopTextWrapPos();

            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("Source: %s (%s)", fx.file_path.c_str(), fx.engine_type.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Separator();
            ImGui::Spacing();

            std::string cur_cat;
            for (size_t u_idx = 0; u_idx < fx.uniforms.size(); ++u_idx) {
                auto &u = fx.uniforms[u_idx];

                if (!u.category.empty() && u.category != cur_cat) {
                    cur_cat = u.category;
                    ImGui::Spacing();
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.88f, 1.0f), "[ %s ]", cur_cat.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::Separator();
                }

                ImGui::PushID(static_cast<int>(u_idx));

                switch (u.type) {
                case ShaderLabUniform::Type::Float: {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(u.label.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::SliderFloat("##val", &u.float_val, u.min_val, u.max_val, "%.3f");
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        u.float_val = u.default_float;
                    }
                    break;
                }
                case ShaderLabUniform::Type::Int: {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(u.label.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::SliderInt("##val", &u.int_val, static_cast<int>(u.min_val), static_cast<int>(u.max_val));
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        u.int_val = u.default_int;
                    }
                    break;
                }
                case ShaderLabUniform::Type::Bool: {
                    ImGui::Checkbox(u.label.c_str(), &u.bool_val);
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        u.bool_val = u.default_bool;
                    }
                    break;
                }
                case ShaderLabUniform::Type::Color3: {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(u.label.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::ColorEdit3("##val", u.color_val);
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        for (int c = 0; c < 3; ++c) u.color_val[c] = u.default_color[c];
                    }
                    break;
                }
                case ShaderLabUniform::Type::Color4: {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(u.label.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::ColorEdit4("##val", u.color_val);
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        for (int c = 0; c < 4; ++c) u.color_val[c] = u.default_color[c];
                    }
                    break;
                }
                case ShaderLabUniform::Type::Enum: {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(u.label.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::SetNextItemWidth(-1.0f);
                    if (!u.enum_items.empty()) {
                        std::vector<const char *> items;
                        for (const auto &item : u.enum_items) items.push_back(item.c_str());
                        ImGui::Combo("##val", &u.enum_selected, items.data(), static_cast<int>(items.size()));
                    }
                    break;
                }
                }

                if (!u.tooltip.empty() && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\nDouble-click to reset to default", u.tooltip.c_str());
                }

                ImGui::PopID();
                ImGui::Spacing();
            }
        } else {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("Select an effect on the left to configure its parameters.");
            ImGui::PopTextWrapPos();
        }
    } else if (gui.m_right_panel_tab == 1) {
        // --- TAB 1: 3D DEPTH ---
        if (!gui.m_has_depth) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("No 3D depth map.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }

        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "CALIBRATION & PREVIEW");
        ImGui::PopTextWrapPos();
        ImGui::Separator();

        bool depth_peek = gui.is_live_depth_peek();
        if (ImGui::Checkbox("Pin Depth Preview", &depth_peek)) {
            gui.set_depth_peek_active(depth_peek);
        }
        gui.register_dom_last_item("depth_checkbox_peek", "Pin Depth Preview", "checkbox", depth_peek);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep depth map visible when switching to other tabs");

        ImGui::Checkbox("Invert Depth Buffer", &gui.m_depth_invert);
        gui.register_dom_last_item("depth_checkbox_invert", "Invert Depth Buffer", "checkbox", gui.m_depth_invert);
        ImGui::Spacing();

        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("Far Clipping Plane");
        ImGui::PopTextWrapPos();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##FarPlane", &gui.m_depth_far_plane, 10.0f, 5000.0f, "%.0f");
        gui.register_dom_last_item("depth_slider_far", "Far Clipping Plane", "slider");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adjusts far depth distance calibration");
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) gui.m_depth_far_plane = 1000.0f;
        ImGui::Spacing();

        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "MACHINE LEARNING ESTIMATION");
        ImGui::PopTextWrapPos();
        ImGui::Separator();

        ImGui::SetNextItemWidth(-1.0f);
        const char *models[] = { "Large (~1.3GB ViT-L)", "Base (~390MB ViT-B)", "Small (~100MB ViT-S)" };
        static int selected_model = 0;
        if (ImGui::Combo("##AIModel", &selected_model, models, 3)) {
            if (selected_model == 0) gui.m_depth_encoder = "vitl";
            else if (selected_model == 1) gui.m_depth_encoder = "vitb";
            else gui.m_depth_encoder = "vits";
        }
        gui.register_dom_last_item("depth_combo_model", "Machine Learning Depth Estimation", "combo");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select Depth-Anything-V2 neural encoder checkpoint:\nViT-L: Maximum depth precision (1.3GB)\nViT-B: Balanced accuracy/speed (390MB)\nViT-S: Fastest inference (100MB)");

        auto &req = ToolRequirements::get();
        if (req.is_depth_downloading()) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "%s", req.get_depth_download_status().c_str());
            ImGui::PopTextWrapPos();
        } else if (!gui.m_depth_status_text.empty() && !gui.m_depth_is_busy) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", gui.m_depth_status_text.c_str());
            ImGui::PopTextWrapPos();
        }
    } else if (gui.m_right_panel_tab == 2) {
        // --- TAB 2: ERASE & INPAINT ---
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "ERASE BRUSH TOOL");
        ImGui::PopTextWrapPos();
        ImGui::Separator();

        bool erase_active = (gui.m_active_tool == ActiveTool::Erase && gui.m_erase_active);
        if (ImGui::Checkbox("Activate Erase Brush (E)", &erase_active)) {
            gui.set_active_tool(erase_active ? ActiveTool::Erase : ActiveTool::Hand);
        }
        gui.register_dom_last_item("erase_checkbox_activate", "Activate Erase Brush", "checkbox", erase_active);
        ImGui::Spacing();

        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("Brush Radius ( [ / ] )");
        ImGui::PopTextWrapPos();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##Radius", &gui.m_erase_radius, 4.0f, 128.0f, "%.0f px")) {
            EraseTool::get().brush_radius = gui.m_erase_radius;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            gui.m_erase_radius = 24.0f;
            EraseTool::get().brush_radius = 24.0f;
        }
        gui.register_dom_last_item("erase_slider_radius", "Brush Radius", "slider");

        ImGui::Checkbox("Subtract Mask Mode (Alt)", &gui.m_erase_subtract);
        gui.register_dom_last_item("erase_checkbox_subtract", "Subtract Mask Mode", "checkbox", gui.m_erase_subtract);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Erase/subtract from current painted mask.\nHold Alt while painting to invert brush temporarily.");

        auto &erase_tool = EraseTool::get();
        ImGui::Checkbox("Auto-apply on stroke release", &erase_tool.auto_apply);
        gui.register_dom_last_item("erase_checkbox_auto_apply", "Auto-apply on stroke release", "checkbox", erase_tool.auto_apply);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatically run LaMa inpainting right after releasing a mask stroke.\nTurn off to paint multiple strokes or press Enter / Inpaint manually.");
        ImGui::Spacing();

        bool is_erasing_busy = gui.m_inpaint_is_busy || erase_tool.is_processing();
        float half_btn = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        bool can_undo = erase_tool.can_undo_stroke() && !is_erasing_busy;
        if (!can_undo) ImGui::BeginDisabled();
        if (ImGui::Button("Undo Stroke (Ctrl+Z)", ImVec2(half_btn, 24.0f))) {
            gui.undo_erase_stroke();
        }
        gui.register_dom_last_item("erase_btn_undo", "Undo Stroke", "button", false, can_undo);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Undo last painted mask stroke (Ctrl+Z)");
        if (!can_undo) ImGui::EndDisabled();
        ImGui::SameLine();
        bool has_mask_edits = erase_tool.has_mask_edits() && !is_erasing_busy;
        if (!has_mask_edits) ImGui::BeginDisabled();
        if (ImGui::Button("Clear Mask (Esc)", ImVec2(half_btn, 24.0f))) {
            gui.clear_erase_strokes();
            gui.m_req_clear_mask = true;
        }
        gui.register_dom_last_item("erase_btn_clear", "Clear Mask", "button", false, has_mask_edits);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Clear the entire painted mask (Esc)");
        if (!has_mask_edits) ImGui::EndDisabled();
        ImGui::Spacing();

        auto &req = ToolRequirements::get();
        if (erase_tool.is_processing()) {
            float p = erase_tool.get_progress();
            std::string st = erase_tool.get_status();
            ImGui::ProgressBar(p >= 0.0f ? p : -1.0f, ImVec2(-1.0f, 20.0f), st.empty() ? "Inpainting..." : st.c_str());
            ImGui::Spacing();
        } else if (erase_tool.is_downloading() || req.is_lama_downloading()) {
            float dp = erase_tool.is_downloading() ? erase_tool.get_download_progress() : req.get_lama_download_progress();
            std::string dl_st = erase_tool.is_downloading() ? erase_tool.get_download_status() : req.get_lama_download_status();
            ImGui::ProgressBar(dp > 0.0f ? dp : -1.0f, ImVec2(-1.0f, 20.0f), dl_st.empty() ? "Downloading LaMa Big HD..." : dl_st.c_str());
            ImGui::Spacing();
        } else {
            std::string last_err = erase_tool.get_last_error();
            if (!last_err.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Error: %s", last_err.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Spacing();
            } else if (!gui.m_inpaint_status_text.empty() && !gui.m_inpaint_is_busy) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", gui.m_inpaint_status_text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Spacing();
            }
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "ERASE HISTORY");
        ImGui::PopTextWrapPos();
        ImGui::Separator();

        size_t cur_step = erase_tool.get_current_erase_step();
        size_t total_steps = erase_tool.get_total_erase_steps();
        ImGui::PushTextWrapPos(0.0f);
        if (total_steps > 0) {
            ImGui::Text("Stage %u of %u", static_cast<unsigned>(cur_step), static_cast<unsigned>(total_steps));
        } else {
            ImGui::TextDisabled("No erase edits yet.");
        }
        ImGui::PopTextWrapPos();

        bool can_undo_erase = erase_tool.can_undo_erase() && !is_erasing_busy;
        if (!can_undo_erase) ImGui::BeginDisabled();
        if (ImGui::Button("Undo Erase", ImVec2(half_btn, 24.0f))) {
            erase_tool.undo_erase();
        }
        gui.register_dom_last_item("erase_btn_undo_erase", "Undo Erase", "button", false, can_undo_erase);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Step backward to previous image stage");
        if (!can_undo_erase) ImGui::EndDisabled();
        ImGui::SameLine();

        bool can_redo_erase = erase_tool.can_redo_erase() && !is_erasing_busy;
        if (!can_redo_erase) ImGui::BeginDisabled();
        if (ImGui::Button("Redo Erase", ImVec2(half_btn, 24.0f))) {
            erase_tool.redo_erase();
        }
        gui.register_dom_last_item("erase_btn_redo_erase", "Redo Erase", "button", false, can_redo_erase);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Step forward to next image stage");
        if (!can_redo_erase) ImGui::EndDisabled();

        if (gui.m_has_depth) {
            ImGui::Spacing();
            ImGui::Separator();
            bool depth_busy = gui.m_depth_is_busy || DepthManager::get().is_processing();
            if (depth_busy) ImGui::BeginDisabled();
            if (ImGui::Button("Re-generate Depth Buffer", ImVec2(-1.0f, 24.0f))) {
                gui.m_req_generate_depth = true;
                gui.m_depth_status_text = "Running Depth-Anything-V2 (" + gui.m_depth_encoder + ")...";
                gui.m_depth_is_busy = true;
            }
            gui.register_dom_last_item("erase_btn_regen_depth", "Re-generate Depth Buffer", "button", false, !depth_busy);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Update 3D depth map after erasing objects");
            if (depth_busy) ImGui::EndDisabled();
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // 4. Pinned Bottom Action Button (24px height, matches left panel buttons)
    if (gui.m_right_panel_tab == 0) {
        bool has_effect = (gui.m_selected_effect_idx >= 0 && gui.m_selected_effect_idx < static_cast<int>(gui.m_effects.size()));
        if (!has_effect) ImGui::BeginDisabled();
        if (ImGui::Button("Reset Parameters to Default", ImVec2(-1.0f, bottom_btn_h))) {
            if (has_effect) gui.m_effects[gui.m_selected_effect_idx].reset_uniforms();
        }
        gui.register_dom_last_item("btn_reset_defaults", "Reset Parameters to Default", "button", false, has_effect);
        if (!has_effect) ImGui::EndDisabled();
    } else if (gui.m_right_panel_tab == 1) {
        auto &depth_mgr = DepthManager::get();
        auto &req = ToolRequirements::get();
        bool is_downloading = depth_mgr.is_downloading() || req.is_depth_downloading();
        bool model_present = depth_mgr.is_model_present(gui.m_depth_encoder) || req.is_depth_model_present(gui.m_depth_encoder);
        bool is_busy = gui.m_depth_is_busy || depth_mgr.is_processing();

        if (is_busy) {
            ImGui::BeginDisabled();
            std::string status = !depth_mgr.get_status().empty() ? depth_mgr.get_status() : gui.m_depth_status_text;
            if (status.empty()) status = "Generating Depth...";
            ImGui::Button(status.c_str(), ImVec2(-1.0f, bottom_btn_h));
            ImGui::EndDisabled();
            gui.register_dom_last_item("depth_btn_generate", "Generating Depth...", "button", false, false);
        } else if (is_downloading) {
            float p = depth_mgr.is_downloading() ? depth_mgr.get_download_progress() : req.get_depth_download_progress();
            char dl_buf[128];
            if (p > 0.0f) snprintf(dl_buf, sizeof(dl_buf), "Downloading: %.0f%%", p * 100.0f);
            else snprintf(dl_buf, sizeof(dl_buf), "Downloading Depth Model...");
            ImGui::BeginDisabled();
            ImGui::Button(dl_buf, ImVec2(-1.0f, bottom_btn_h));
            ImGui::EndDisabled();
            gui.register_dom_last_item("depth_btn_generate", dl_buf, "button", false, false);
        } else if (!model_present) {
            const char *dl_label = "Download & Generate Depth (1.3GB)";
            if (gui.m_depth_encoder == "vitb") dl_label = "Download & Generate Depth (390MB)";
            else if (gui.m_depth_encoder == "vits") dl_label = "Download & Generate Depth (100MB)";

            if (ImGui::Button(dl_label, ImVec2(-1.0f, bottom_btn_h))) {
                if (!depth_mgr.trigger_model_download(gui.m_depth_encoder)) {
                    req.trigger_depth_download(gui.m_depth_encoder);
                }
            }
            gui.register_dom_last_item("depth_btn_generate", dl_label, "button");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Download Depth Anything V2 (%s) model checkpoint", gui.m_depth_encoder.c_str());
        } else {
            if (ImGui::Button("Generate 3D Depth Map", ImVec2(-1.0f, bottom_btn_h))) {
                gui.m_req_generate_depth = true;
                gui.m_depth_status_text = "Running Depth-Anything-V2 (" + gui.m_depth_encoder + ")...";
                gui.m_depth_is_busy = true;
            }
            gui.register_dom_last_item("depth_btn_generate", "Generate 3D Depth Map", "button");
        }
    } else if (gui.m_right_panel_tab == 2) {
        auto &erase_tool = EraseTool::get();
        auto &req = ToolRequirements::get();
        bool is_downloading = erase_tool.is_downloading() || req.is_lama_downloading();
        bool model_present = erase_tool.is_model_present() || req.is_lama_model_present();
        bool is_busy = gui.m_inpaint_is_busy || erase_tool.is_processing();

        if (is_busy) {
            ImGui::BeginDisabled();
            std::string status = !erase_tool.get_status().empty() ? erase_tool.get_status() : gui.m_inpaint_status_text;
            if (status.empty()) status = "Inpainting...";
            ImGui::Button(status.c_str(), ImVec2(-1.0f, bottom_btn_h));
            ImGui::EndDisabled();
            gui.register_dom_last_item("erase_btn_inpaint", "Inpainting...", "button", false, false);
        } else if (is_downloading) {
            float p = erase_tool.is_downloading() ? erase_tool.get_download_progress() : req.get_lama_download_progress();
            char dl_buf[128];
            if (p > 0.0f) snprintf(dl_buf, sizeof(dl_buf), "Downloading: %.0f%%", p * 100.0f);
            else snprintf(dl_buf, sizeof(dl_buf), "Downloading LaMa Big HD (~196MB)...");
            ImGui::BeginDisabled();
            ImGui::Button(dl_buf, ImVec2(-1.0f, bottom_btn_h));
            ImGui::EndDisabled();
            gui.register_dom_last_item("erase_btn_inpaint", dl_buf, "button", false, false);
        } else if (!model_present) {
            if (ImGui::Button("Download LaMa Big HD (196MB)", ImVec2(-1.0f, bottom_btn_h))) {
                if (!erase_tool.trigger_model_download()) {
                    req.trigger_lama_download();
                }
            }
            gui.register_dom_last_item("erase_btn_inpaint", "Download LaMa Big HD (196MB)", "button");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Download neural inpainting model (common/big-lama.pt) required for object erasing");
        } else {
            bool has_mask = erase_tool.has_mask_edits();
            if (!has_mask) ImGui::BeginDisabled();
            if (ImGui::Button("Inpaint Selection (LaMa)", ImVec2(-1.0f, bottom_btn_h))) {
                gui.request_inpaint();
            }
            gui.register_dom_last_item("erase_btn_inpaint", "Inpaint Selection", "button", false, has_mask);
            if (!has_mask) ImGui::EndDisabled();
        }
    }

    // Left border drawn in panel window draw list
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(right_offset + 0.5f, top_offset),
                  ImVec2(right_offset + 0.5f, top_offset + panel_height),
                  gui.m_resizing_right ? IM_COL32(75, 140, 220, 240) : IM_COL32(36, 36, 40, 220), gui.m_resizing_right ? 2.0f : 1.0f);

    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace gui
