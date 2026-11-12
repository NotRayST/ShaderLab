#include "exit_modal.h"
#include <algorithm>
#include <string>

namespace gui {

ExitModalDecision render_unsaved_exit_modal(
    bool &open_requested,
    bool &out_is_open,
    const std::vector<DocumentTab> &tabs,
    UiDomRegistry *dom
) {
    constexpr const char *kModalId = "Unsaved Changes###UnsavedExitModal";

    if (open_requested) {
        ImGui::OpenPopup(kModalId);
        open_requested = false;
    }

    ExitModalDecision decision = ExitModalDecision::None;

    if (ImGuiViewport *vp = ImGui::GetMainViewport()) {
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Always);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings;

    out_is_open = false;
    if (ImGui::BeginPopupModal(kModalId, nullptr, flags)) {
        out_is_open = true;

        if (dom) {
            const ImVec2 wpos = ImGui::GetWindowPos();
            const ImVec2 wsz = ImGui::GetWindowSize();
            dom->register_item("modal_unsaved_exit", "Unsaved Changes", "modal", wpos, ImVec2(wpos.x + wsz.x, wpos.y + wsz.y), true, true);
        }

        ImGui::TextWrapped("You have unsaved changes. Save before closing?");
        ImGui::Spacing();

        std::vector<std::string> dirty_names;
        for (const auto &tab : tabs) {
            if (tab.is_dirty) {
                std::string nm = to_utf8(tab.display_name);
                if (nm.empty()) nm = "Untitled";
                dirty_names.push_back(nm);
            }
        }
        if (dirty_names.empty()) {
            dirty_names.push_back("Active document");
        }

        const float list_h = std::min(120.0f, static_cast<float>(dirty_names.size()) * (ImGui::GetTextLineHeightWithSpacing() + 2.0f) + 8.0f);
        if (ImGui::BeginChild("UnsavedDocList", ImVec2(-1.0f, list_h), true)) {
            for (const auto &name : dirty_names) {
                BulletTextWrapped("%s", name.c_str());
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float btn_w = std::max(90.0f, (avail_w - spacing_x * 2.0f) / 3.0f);

        // Primary Save action
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.173f, 0.427f, 0.722f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.220f, 0.502f, 0.820f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.145f, 0.365f, 0.635f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("Save", ImVec2(btn_w, 0.0f))) {
            decision = ExitModalDecision::SaveAndExit;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(4);
        if (dom) {
            dom->register_item("modal_exit_btn_save", "Save", "button", ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false, true);
        }

        ImGui::SameLine();

        // Discard action
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.56f, 0.19f, 0.19f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.24f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.48f, 0.14f, 0.14f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("Don't Save", ImVec2(btn_w, 0.0f))) {
            decision = ExitModalDecision::DontSaveAndExit;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(4);
        if (dom) {
            dom->register_item("modal_exit_btn_dont_save", "Don't Save", "button", ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false, true);
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(btn_w, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            decision = ExitModalDecision::Cancel;
            ImGui::CloseCurrentPopup();
        }
        if (dom) {
            dom->register_item("modal_exit_btn_cancel", "Cancel", "button", ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false, true);
        }

        ImGui::EndPopup();
    }

    return decision;
}

} // namespace gui
