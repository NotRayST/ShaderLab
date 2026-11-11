#include "tab_bar.h"
#include "gui_manager.h"
#include <imgui.h>

namespace gui {

void render_tab_bar(GuiManager &gui, float x_offset, float top_offset, float width) {
    ImGui::SetNextWindowPos(ImVec2(x_offset, top_offset));
    ImGui::SetNextWindowSize(ImVec2(width, gui.get_tab_bar_height()));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                            ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 3.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.13f, 1.0f));
    ImGui::Begin("##PhotoshopDocumentTabBar", nullptr, flags);

    ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_Reorderable |
                                     ImGuiTabBarFlags_AutoSelectNewTabs |
                                     ImGuiTabBarFlags_FittingPolicyMixed |
                                     ImGuiTabBarFlags_NoCloseWithMiddleMouseButton;

    if (ImGui::BeginTabBar("##DocumentTabs", tab_bar_flags)) {
        for (int i = 0; i < static_cast<int>(gui.m_tabs.size()); ++i) {
            auto &tab = gui.m_tabs[i];
            bool open = true;
            ImGuiTabItemFlags item_flags = 0;
            if (gui.m_programmatic_select_tab == i) {
                item_flags |= ImGuiTabItemFlags_SetSelected;
            }
            if (tab.is_dirty) {
                item_flags |= ImGuiTabItemFlags_UnsavedDocument;
            }

            std::string title_u8 = to_utf8(tab.display_name.empty() ? L"Untitled" : tab.display_name);
            std::string label = title_u8 + "###tab_doc_" + std::to_string(tab.id);

            bool is_selected = ImGui::BeginTabItem(label.c_str(), &open, item_flags);
            if (is_selected) {
                if (gui.m_programmatic_select_tab < 0 && gui.m_active_tab != i) {
                    gui.m_req_switch_tab = i;
                    gui.m_active_tab = i;
                }
                gui.register_dom_last_item("tab_doc_" + std::to_string(tab.id), title_u8, "tab", true);
            } else {
                gui.register_dom_last_item("tab_doc_" + std::to_string(tab.id), title_u8, "tab", false);
            }

            if (!open) {
                gui.m_req_close_tab = i;
            }

            if (ImGui::IsItemHovered()) {
                if (ImGui::IsMouseClicked(2)) {
                    gui.m_req_close_tab = i;
                }

                ImGui::BeginTooltip();
                ImGui::TextUnformatted(title_u8.c_str());
                if (tab.width > 0 && tab.height > 0) {
                    ImGui::TextDisabled("Dimensions: %ux%u", tab.width, tab.height);
                }
                if (tab.has_depth) {
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "3D Depth: Available");
                }
                if (!tab.file_path.empty()) {
                    std::string p_u8 = to_utf8(tab.file_path);
                    ImGui::TextDisabled("Path: %s", p_u8.c_str());
                }
                ImGui::EndTooltip();
            }

            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Close Tab", "Ctrl+W")) {
                    gui.m_req_close_tab = i;
                }
                if (ImGui::MenuItem("Close Others")) {
                    gui.m_req_close_other_tabs = i;
                }
                if (ImGui::MenuItem("Close All")) {
                    gui.m_req_close_all_tabs = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Duplicate Tab")) {
                    gui.m_req_duplicate_tab = i;
                }
                if (!tab.file_path.empty()) {
                    if (ImGui::MenuItem("Reveal in File Explorer")) {
                        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + tab.file_path + L"\"").c_str(), nullptr, SW_SHOW);
                    }
                }
                ImGui::EndPopup();
            }

            if (is_selected) {
                ImGui::EndTabItem();
            }
        }

        gui.m_programmatic_select_tab = -1;

        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing)) {
            std::wstring path;
            if (open_file_dialog(gui.m_hwnd, L"Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files (*.*)\0*.*\0", path)) {
                gui.m_req_open_image = true;
                gui.m_open_image_path = path;
            }
        }
        gui.register_dom_last_item("tab_btn_add", "+", "button");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Open image in new tab (Ctrl+O)");
        }

        ImGui::EndTabBar();
    }

    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(x_offset, top_offset + gui.get_tab_bar_height() - 0.5f),
                  ImVec2(x_offset + width, top_offset + gui.get_tab_bar_height() - 0.5f),
                  IM_COL32(36, 36, 40, 220), 1.0f);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace gui
