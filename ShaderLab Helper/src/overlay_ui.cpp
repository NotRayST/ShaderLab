#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "overlay_ui.h"
#include "job_queue.h"
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static char s_output_folder[1024] = "out";
static bool s_show_logs = false;
static char s_prev_output_folder[1024] = "out"; // dirty check so we only log actual changes

static std::wstring utf8_to_wide(const char *utf8_str) {
    if (!utf8_str || !*utf8_str) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring out(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, out.data(), size);
    return out;
}

static std::string wide_to_utf8_ui(const wchar_t *wstr) {
    if (!wstr || !*wstr) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), size, nullptr, nullptr);
    return out;
}

static bool browse_image_dialog(std::wstring &selected_path) {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.lpstrFilter = L"Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        selected_path = filename;
        return true;
    }
    return false;
}

void on_overlay(reshade::api::effect_runtime *) {
    JobQueueManager &queue_mgr = JobQueueManager::get();
    queue_mgr.update();

    const std::wstring &active_path = queue_mgr.get_active_image_path();
    ExportState state = queue_mgr.get_export_state();
    bool is_exporting = (state == ExportState::Requested ||
                         state == ExportState::Loading ||
                         state == ExportState::Rendering ||
                         state == ExportState::Capturing);

    // image source: big drop zone, click to open a file dialog
    ImGui::Spacing();
    if (ImGui::Button("[ Drag & Drop Image Here or Click to Browse ]", ImVec2(-1, 55))) {
        std::wstring picked;
        if (browse_image_dialog(picked)) {
            queue_mgr.set_preview_image(picked);
            std::string name = wide_to_utf8_ui(fs::path(picked).filename().wstring().c_str());
            queue_mgr.add_log(L"Image loaded: " + fs::path(picked).filename().wstring());
        }
    }

    // files dropped on the host window come back to us through the block
    SharedControlBlock *block = queue_mgr.get_control_block();
    if (block) {
        static uint32_t s_last_preview_counter = 0;
        if (block->preview_counter != s_last_preview_counter) {
            s_last_preview_counter = block->preview_counter;
            if (wcslen(block->preview_path) > 0) {
                queue_mgr.add_log(L"Image loaded: " + std::wstring(block->preview_path));
            }
        }
    }

    if (!active_path.empty()) {
        ImGui::TextDisabled("File: %s", wide_to_utf8_ui(active_path.c_str()).c_str());
    } else {
        ImGui::TextDisabled("File: None (drop an image or click above)");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // where exports go. "Open" just spawns explorer, dont overthink it
    ImGui::InputText("Output Folder", s_output_folder, sizeof(s_output_folder));
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        std::wstring out_w = utf8_to_wide(s_output_folder);
        if (out_w.empty()) out_w = L"out";
        fs::path p(out_w);
        if (!fs::exists(p)) {
            std::error_code ec;
            fs::create_directories(p, ec);
        }
        ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOW);
    }

    // only log the folder line once per edit (fires on enter/blur)
    if (ImGui::IsItemDeactivatedAfterEdit() || strcmp(s_output_folder, s_prev_output_folder) != 0) {
        if (strcmp(s_output_folder, s_prev_output_folder) != 0) {
            queue_mgr.add_log(L"Output folder set to: " + utf8_to_wide(s_output_folder));
            strncpy_s(s_prev_output_folder, s_output_folder, sizeof(s_prev_output_folder));
        }
    }

    ImGui::Spacing();

    // the whole point: render current image through active shaders, dump result
    ImGui::BeginDisabled(active_path.empty() || is_exporting);
    if (ImGui::Button(is_exporting ? "Exporting..." : "Export Image", ImVec2(-1, 35))) {
        std::wstring out_w = utf8_to_wide(s_output_folder);
        queue_mgr.add_log(L"Export started: " + fs::path(active_path).filename().wstring()
                         + L" -> " + (out_w.empty() ? L"out" : out_w));
        queue_mgr.trigger_export(active_path, out_w);
    }
    ImGui::EndDisabled();

    // surface whatever the host wrote into export_status (ok or error, same field)
    if (is_exporting) {
        ImGui::TextDisabled("Rendering and capturing effects...");
    } else if (block && wcslen(block->export_status) > 0) {
        if (block->export_error == IPC_OK) {
            ImGui::TextDisabled("%s", wide_to_utf8_ui(block->export_status).c_str());
        } else {
            ImGui::TextDisabled("%s", wide_to_utf8_ui(block->export_status).c_str());
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // collapsible log pane, ring buffer is capped so this stays cheap
    if (ImGui::Button(s_show_logs ? "Hide Logs" : "View Logs")) {
        s_show_logs = !s_show_logs;
    }

    if (s_show_logs) {
        ImGui::Spacing();
        ImGui::BeginChild("LogWindow", ImVec2(0, 120), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto &line : queue_mgr.get_logs()) {
            std::string s = wide_to_utf8_ui(line.c_str());
            ImGui::TextUnformatted(s.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // footer
    ImGui::TextDisabled("ShaderLab  -  by NotRaySt");
    ImGui::SameLine();
    if (ImGui::SmallButton("Twitter")) {
        ShellExecuteW(nullptr, L"open", L"https://x.com/NotRay_st", nullptr, nullptr, SW_SHOW);
    }
}
