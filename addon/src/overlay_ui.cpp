#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "overlay_ui.h"
#include "job_queue.h"
#include "undo_history.h"
#include "keybinds.h"
#include "before_after.h"
#include "depth_manager.h"
#include "project_file.h"
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;

static char s_output_folder[1024] = "ShaderLab Exported";
static bool s_show_logs = false;
static char s_prev_output_folder[1024] = "ShaderLab Exported";

// export settings
static float s_settle_time_sec = 15.0f;
static bool  s_auto_converge = true;
static bool  s_export_wysiwyg = false;
static bool  s_export_embed_depth = false;
static int   s_depth_export_format = 0; // 0 = Embed in PNG, 1 = Sidecar (.sldepth), 2 = Both

static wchar_t s_selected_depth_path[MAX_PATH] = {};
static bool   s_ss_invert = false;
static float  s_ss_far_plane = 1000.0f;

static std::wstring s_current_project_path;

#include "../../common/str_utils.h"

using str_utils::utf8_to_wide;
using str_utils::wide_to_utf8;
inline std::string wide_to_utf8_ui(const wchar_t *wstr) { return str_utils::wide_to_utf8(wstr); }
inline std::string wide_to_utf8_ui(const std::wstring &wstr) { return str_utils::wide_to_utf8(wstr); }

static bool browse_image_dialog(std::wstring &selected_path) {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.lpstrFilter = L"All Supported Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr;*.shaderlab;*.slab)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr;*.shaderlab;*.slab\0ShaderLab Projects (*.shaderlab;*.slab)\0*.shaderlab;*.slab\0Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        selected_path = filename;
        return true;
    }
    return false;
}

// prompt windows file explorer to save project file
static bool browse_save_project_dialog(
    HWND hwnd,
    const std::wstring &suggested_stem,
    const std::wstring &initial_dir,
    std::wstring &out_selected_path
) {
    wchar_t filename[kMaxPathW] = {};
    if (!suggested_stem.empty()) {
        std::wstring def_name = suggested_stem;
        if (!def_name.ends_with(L".shaderlab")) {
            def_name += L".shaderlab";
        }
        wcsncpy_s(filename, kMaxPathW, def_name.c_str(), _TRUNCATE);
    }

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"ShaderLab Project (*.shaderlab)\0*.shaderlab\0All Files (*.*)\0*.*\0";
    ofn.lpstrDefExt = L"shaderlab";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = kMaxPathW;
    if (!initial_dir.empty() && fs::exists(initial_dir)) {
        ofn.lpstrInitialDir = initial_dir.c_str();
    }
    ofn.lpstrTitle = L"Save Project (.shaderlab)";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

    if (GetSaveFileNameW(&ofn)) {
        out_selected_path = filename;
        return true;
    }
    return false;
}

static bool browse_export_as_dialog(
    HWND hwnd,
    const std::wstring &suggested_stem,
    const std::wstring &initial_dir,
    std::wstring &out_selected_path
) {
    wchar_t filename[kMaxPathW] = {};
    if (!suggested_stem.empty()) {
        std::wstring def_name = suggested_stem;
        if (!def_name.ends_with(L".png") && !def_name.ends_with(L".jpg") && !def_name.ends_with(L".jpeg") && !def_name.ends_with(L".bmp")) {
            def_name += L".png";
        }
        wcsncpy_s(filename, kMaxPathW, def_name.c_str(), _TRUNCATE);
    }

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"PNG Image (*.png)\0*.png\0JPEG Image (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0Bitmap (*.bmp)\0*.bmp\0All Files (*.*)\0*.*\0";
    ofn.lpstrDefExt = L"png";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = kMaxPathW;
    if (!initial_dir.empty() && fs::exists(initial_dir)) {
        ofn.lpstrInitialDir = initial_dir.c_str();
    }
    ofn.lpstrTitle = L"Export Image As";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

    if (GetSaveFileNameW(&ofn)) {
        std::wstring res = filename;
        fs::path p(res);
        if (!p.has_extension()) {
            if (ofn.nFilterIndex == 2) res += L".jpg";
            else if (ofn.nFilterIndex == 3) res += L".bmp";
            else res += L".png";
        }
        out_selected_path = res;
        return true;
    }
    return false;
}

static bool browse_folder_dialog(HWND hwnd, const std::wstring &initial_dir, std::wstring &out_folder_path) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool need_uninit = SUCCEEDED(hr);

    IFileOpenDialog *pfd = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (FAILED(hr)) {
        if (need_uninit) CoUninitialize();
        return false;
    }

    DWORD dwOptions = 0;
    pfd->GetOptions(&dwOptions);
    pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    pfd->SetTitle(L"Select Output Folder");

    if (!initial_dir.empty() && fs::exists(initial_dir)) {
        IShellItem *psiFolder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial_dir.c_str(), nullptr, IID_PPV_ARGS(&psiFolder)))) {
            pfd->SetFolder(psiFolder);
            psiFolder->Release();
        }
    }

    hr = pfd->Show(hwnd);
    if (SUCCEEDED(hr)) {
        IShellItem *psiResult = nullptr;
        if (SUCCEEDED(pfd->GetResult(&psiResult))) {
            PWSTR pszPath = nullptr;
            if (SUCCEEDED(psiResult->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                out_folder_path = pszPath;
                CoTaskMemFree(pszPath);
                psiResult->Release();
                pfd->Release();
                if (need_uninit) CoUninitialize();
                return true;
            }
            psiResult->Release();
        }
    }

    pfd->Release();
    if (need_uninit) CoUninitialize();
    return false;
}


static void BulletTextWrapped(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextWrapped("%s", buf);
    ImGui::PopTextWrapPos();
}

static bool DrawProgressButton(const char *id, const char *label, float progress, bool is_active, bool is_disabled, ImVec2 size) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (size.x <= 0.0f) size.x = avail.x;
    if (size.y <= 0.0f) size.y = ImGui::GetFrameHeight();

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    ImGuiStyle &style = ImGui::GetStyle();

    ImGui::InvisibleButton(id, size);
    bool clicked = ImGui::IsItemClicked() && !is_disabled && !is_active;
    bool hovered = ImGui::IsItemHovered() && !is_disabled;

    ImU32 bg_col = ImGui::GetColorU32(is_disabled ? ImGuiCol_Button : (hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button));
    ImU32 border_col = ImGui::GetColorU32(ImGuiCol_Border);
    float rounding = 6.0f; // rounded rectangle squircle

    // base fill
    draw_list->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg_col, rounding);

    // progress overlay
    if (is_active) {
        if (progress >= 0.0f) {
            float fill_w = std::clamp(progress, 0.0f, 1.0f) * size.x;
            if (fill_w > 0.0f) {
                ImU32 fill_col = ImColor(45, 125, 215, 220);
                draw_list->AddRectFilled(pos, ImVec2(pos.x + fill_w, pos.y + size.y), fill_col, rounding);

                // liveness pulse streak through the filled portion
                if (progress < 1.0f && fill_w > 12.0f) {
                    float t = static_cast<float>(fmod(ImGui::GetTime() * 1.3, 1.0));
                    float streak_w = (std::max)(fill_w * 0.35f, 20.0f);
                    float start_x = pos.x + (t * (fill_w + streak_w)) - streak_w;
                    float end_x = start_x + streak_w;
                    float cl_start = (std::max)(start_x, pos.x);
                    float cl_end = (std::min)(end_x, pos.x + fill_w);
                    if (cl_end > cl_start) {
                        ImU32 streak_col = ImColor(110, 195, 255, 130);
                        draw_list->AddRectFilled(ImVec2(cl_start, pos.y), ImVec2(cl_end, pos.y + size.y), streak_col, rounding);
                    }
                }
            }
        } else {
            // indeterminate shimmer for when no progress value is available
            float t = static_cast<float>(fmod(ImGui::GetTime() * 1.2, 1.0));
            float bar_w = size.x * 0.35f;
            float start_x = pos.x + (t * (size.x + bar_w)) - bar_w;
            float end_x = start_x + bar_w;
            float cl_start = std::max(start_x, pos.x);
            float cl_end = std::min(end_x, pos.x + size.x);
            if (cl_end > cl_start) {
                ImU32 shimmer_col = ImColor(60, 140, 220, 160);
                draw_list->AddRectFilled(ImVec2(cl_start, pos.y), ImVec2(cl_end, pos.y + size.y), shimmer_col, rounding);
            }
        }
    }

    // border
    if (style.FrameBorderSize > 0.0f) {
        draw_list->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), border_col, rounding, 0, style.FrameBorderSize);
    }

    // centered label, clipped to button rect
    ImVec2 text_size = ImGui::CalcTextSize(label);
    float text_x = pos.x + (size.x - text_size.x) * 0.5f;
    if (text_x < pos.x + 4.0f) text_x = pos.x + 4.0f;
    ImVec2 text_pos = ImVec2(text_x, pos.y + (size.y - text_size.y) * 0.5f);
    ImU32 text_col = ImGui::GetColorU32(is_disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text);

    ImVec4 clip_rect(pos.x + 2.0f, pos.y, pos.x + size.x - 2.0f, pos.y + size.y);
    draw_list->AddText(nullptr, 0.0f, text_pos, text_col, label, nullptr, 0.0f, &clip_rect);

    return clicked;
}

bool execute_project_save(
    reshade::api::effect_runtime *runtime,
    const std::wstring &active_path,
    const std::wstring &target_project_path,
    SharedControlBlock *block,
    JobQueueManager &queue_mgr
) {
    try {
        if (active_path.empty()) {
            queue_mgr.add_log(L"Cannot save project: No active image loaded");
            if (block) {
                wcsncpy_s(block->toast_message, kMaxPathW, L"Cannot save: No image loaded", _TRUNCATE);
                block->toast_version++;
            }
            return false;
        }

        project_file::ViewportState v_state;
        if (block) {
            v_state.zoom = block->view_zoom;
            v_state.angle = block->view_angle;
            v_state.pan[0] = static_cast<float>(block->view_pan[0]);
            v_state.pan[1] = static_cast<float>(block->view_pan[1]);
            v_state.is_fullscreen = (block->is_fullscreen != 0);
            v_state.lock_pan = (block->view_interaction_flags & VIEW_FLAG_LOCK_PAN) != 0;
            v_state.lock_zoom = (block->view_interaction_flags & VIEW_FLAG_LOCK_ZOOM) != 0;
            v_state.lock_rot = (block->view_interaction_flags & VIEW_FLAG_LOCK_ROT) != 0;
        }

        project_file::DepthState d_state;
        if (block && block->depth_valid) {
            d_state.has_depth = true;
            d_state.far_plane = block->depth_far_plane;
            d_state.depth_sidecar_path = s_selected_depth_path;
            if (d_state.depth_sidecar_path.empty()) {
                fs::path auto_sc = fs::path(active_path).parent_path() / (fs::path(active_path).stem().wstring() + L".sldepth");
                if (fs::exists(auto_sc)) {
                    d_state.depth_sidecar_path = auto_sc.wstring();
                }
            }
        }

        // Export active ReShade preset to memory buffer
        std::vector<uint8_t> preset_bytes;
        if (runtime) {
            fs::path temp_preset_p = fs::temp_directory_path() / L"shaderlab_temp_preset.ini";
            std::string temp_preset_u8 = temp_preset_p.string();
            try {
                runtime->export_current_preset(temp_preset_u8.c_str());
            } catch (...) {
            }

            std::error_code ec;
            if (fs::exists(temp_preset_p)) {
                std::ifstream in(temp_preset_p, std::ios::binary | std::ios::ate);
                if (in.is_open()) {
                    auto sz = in.tellg();
                    if (sz > 0) {
                        preset_bytes.resize(static_cast<size_t>(sz));
                        in.seekg(0, std::ios::beg);
                        in.read(reinterpret_cast<char *>(preset_bytes.data()), sz);
                    }
                }
                fs::remove(temp_preset_p, ec);
            }

            if (preset_bytes.empty()) {
                char current_preset[MAX_PATH] = {};
                size_t p_size = sizeof(current_preset);
                runtime->get_current_preset_path(current_preset, &p_size);
                if (current_preset[0] != '\0' && fs::exists(current_preset)) {
                    std::ifstream in(current_preset, std::ios::binary | std::ios::ate);
                    if (in.is_open()) {
                        auto sz = in.tellg();
                        if (sz > 0) {
                            preset_bytes.resize(static_cast<size_t>(sz));
                            in.seekg(0, std::ios::beg);
                            in.read(reinterpret_cast<char *>(preset_bytes.data()), sz);
                        }
                    }
                }
            }
        }

        std::wstring err;
        if (project_file::save_project(target_project_path, active_path, preset_bytes, v_state, d_state, err)) {
            s_current_project_path = target_project_path;
            std::wstring fname_w = fs::path(target_project_path).filename().wstring();
            if (block) {
                wcsncpy_s(block->active_project_path, kMaxPathW, target_project_path.c_str(), _TRUNCATE);
                block->project_dirty = 0;
                block->active_project_version++;

                // signal toast to host app ToastHud so notification appears whether overlay is open or closed
                std::wstring toast_w = L"Project saved: " + fname_w;
                wcsncpy_s(block->toast_message, kMaxPathW, toast_w.c_str(), _TRUNCATE);
                block->toast_version++;
            }
            queue_mgr.add_log(L"Project saved: " + target_project_path);
            return true;
        } else {
            queue_mgr.add_log(L"Project save failed: " + err);
            if (block) {
                std::wstring fail_w = L"Project save failed: " + err;
                wcsncpy_s(block->toast_message, kMaxPathW, fail_w.c_str(), _TRUNCATE);
                block->toast_version++;
            }
            return false;
        }
    } catch (const std::exception &ex) {
        queue_mgr.add_log(L"Project save exception: " + utf8_to_wide(ex.what()));
        return false;
    } catch (...) {
        queue_mgr.add_log(L"Project save exception: unexpected error");
        return false;
    }
}

void trigger_save_project_workflow(reshade::api::effect_runtime *runtime, bool force_dialog) {
    static bool s_save_dialog_in_flight = false;
    if (s_save_dialog_in_flight) {
        return;
    }
    struct FlightGuard {
        bool &flag;
        FlightGuard(bool &f) : flag(f) { flag = true; }
        ~FlightGuard() { flag = false; }
    } guard(s_save_dialog_in_flight);

    JobQueueManager &queue_mgr = JobQueueManager::get();
    queue_mgr.update();
    const std::wstring &active_path = queue_mgr.get_active_image_path();
    SharedControlBlock *block = queue_mgr.get_control_block();

    if (active_path.empty()) {
        queue_mgr.add_log(L"Cannot save project: No active image loaded");
        if (block) {
            wcsncpy_s(block->toast_message, kMaxPathW, L"Cannot save: No image loaded", _TRUNCATE);
            block->toast_version++;
        }
        return;
    }

    // if project was already saved or loaded, overwrite it directly on disk without nagging dialog
    if (!force_dialog && !s_current_project_path.empty() && fs::exists(s_current_project_path)) {
        execute_project_save(runtime, active_path, s_current_project_path, block, queue_mgr);
        return;
    }

    // first time save (or Save As): open windows file explorer save dialog
    HWND hwnd = runtime ? static_cast<HWND>(runtime->get_hwnd()) : GetForegroundWindow();

    std::wstring suggested_stem;
    std::wstring initial_dir;

    if (!s_current_project_path.empty()) {
        fs::path cur_p(s_current_project_path);
        suggested_stem = cur_p.stem().wstring();
        initial_dir = cur_p.parent_path().wstring();
    } else {
        if (block && wcslen(block->dropped_file_path) > 0) {
            fs::path orig_p(block->dropped_file_path);
            suggested_stem = orig_p.stem().wstring();
            if (fs::exists(orig_p.parent_path()) &&
                orig_p.parent_path().string().find("ShaderLab\\workspace") == std::string::npos) {
                initial_dir = orig_p.parent_path().wstring();
            }
        }
        if (suggested_stem.empty() && !active_path.empty()) {
            suggested_stem = fs::path(active_path).stem().wstring();
        }
        if (initial_dir.empty() && !active_path.empty()) {
            fs::path act_p(active_path);
            if (act_p.parent_path().string().find("ShaderLab\\workspace") == std::string::npos &&
                fs::exists(act_p.parent_path())) {
                initial_dir = act_p.parent_path().wstring();
            }
        }
        if (initial_dir.empty()) {
            std::wstring out_w = utf8_to_wide(s_output_folder);
            if (fs::exists(out_w)) {
                initial_dir = out_w;
            }
        }
    }

    if (suggested_stem.empty() || suggested_stem == L"preview") {
        suggested_stem = L"project";
    }

    std::wstring chosen_path;
    if (browse_save_project_dialog(hwnd, suggested_stem, initial_dir, chosen_path)) {
        execute_project_save(runtime, active_path, chosen_path, block, queue_mgr);
    }
}

void trigger_export_workflow(reshade::api::effect_runtime *runtime) {
    JobQueueManager &queue_mgr = JobQueueManager::get();
    queue_mgr.update();
    const std::wstring &active_path = queue_mgr.get_active_image_path();
    SharedControlBlock *block = queue_mgr.get_control_block();

    ExportState state = queue_mgr.get_export_state();
    bool is_exporting = (state == ExportState::Requested ||
                         state == ExportState::Loading ||
                         state == ExportState::Rendering ||
                         state == ExportState::Capturing);
    if (is_exporting) {
        queue_mgr.add_log(L"Export already in progress");
        return;
    }

    if (active_path.empty()) {
        queue_mgr.add_log(L"Cannot export: No active image loaded");
        if (block) {
            wcsncpy_s(block->toast_message, kMaxPathW, L"Cannot export: No image loaded", _TRUNCATE);
            block->toast_version++;
        }
        return;
    }

    std::wstring out_w = utf8_to_wide(s_output_folder);
    uint32_t flags = 0;
    if (s_auto_converge)      flags |= EXPORT_FLAG_AUTO_CONVERGE;
    if (s_export_wysiwyg)     flags |= EXPORT_FLAG_WYSIWYG;
    if (s_export_embed_depth) {
        if (s_depth_export_format == 0)      flags |= EXPORT_FLAG_EMBED_DEPTH;
        else if (s_depth_export_format == 1) flags |= EXPORT_FLAG_DEPTH_SIDECAR;
        else                                 flags |= (EXPORT_FLAG_EMBED_DEPTH | EXPORT_FLAG_DEPTH_SIDECAR);
    }

    uint32_t settle_cap = s_auto_converge ? 1800 : static_cast<uint32_t>(std::round(s_settle_time_sec * 60.0f));
    if (settle_cap < 30) settle_cap = 30;

    fs::path in_p(active_path);
    fs::path out_dir(out_w.empty() ? L"ShaderLab Exported" : out_w);
    fs::path planned_file = out_dir / (in_p.stem().wstring() + L".png");

    std::error_code ec;
    bool is_duplicate = fs::exists(planned_file, ec);

    if (is_duplicate) {
        // duplicate detected: open file explorer for export
        HWND hwnd = runtime ? static_cast<HWND>(runtime->get_hwnd()) : GetForegroundWindow();
        std::wstring chosen_path;
        if (browse_export_as_dialog(hwnd, in_p.stem().wstring(), out_dir.wstring(), chosen_path)) {
            fs::path chosen_p(chosen_path);
            std::wstring override_name = chosen_p.filename().wstring();
            std::wstring target_dir = chosen_p.parent_path().wstring();

            queue_mgr.add_log(L"Export started: " + in_p.filename().wstring()
                             + L" -> " + override_name);
            queue_mgr.trigger_export(active_path, target_dir, settle_cap, flags, override_name);
        }
    } else {
        // new image: exports directly into out folder without opening file explorer
        queue_mgr.add_log(L"Export started: " + in_p.filename().wstring()
                         + L" -> " + planned_file.filename().wstring());
        queue_mgr.trigger_export(active_path, out_w, settle_cap, flags, L"");
    }
}

void trigger_export_as_workflow(reshade::api::effect_runtime *runtime) {
    JobQueueManager &queue_mgr = JobQueueManager::get();
    queue_mgr.update();
    const std::wstring &active_path = queue_mgr.get_active_image_path();
    SharedControlBlock *block = queue_mgr.get_control_block();

    ExportState state = queue_mgr.get_export_state();
    bool is_exporting = (state == ExportState::Requested ||
                         state == ExportState::Loading ||
                         state == ExportState::Rendering ||
                         state == ExportState::Capturing);
    if (is_exporting) {
        queue_mgr.add_log(L"Export already in progress");
        return;
    }

    if (active_path.empty()) {
        queue_mgr.add_log(L"Cannot export: No active image loaded");
        if (block) {
            wcsncpy_s(block->toast_message, kMaxPathW, L"Cannot export: No image loaded", _TRUNCATE);
            block->toast_version++;
        }
        return;
    }

    HWND hwnd = runtime ? static_cast<HWND>(runtime->get_hwnd()) : GetForegroundWindow();

    std::wstring suggested_stem;
    std::wstring initial_dir;

    if (block && wcslen(block->dropped_file_path) > 0) {
        fs::path orig_p(block->dropped_file_path);
        suggested_stem = orig_p.stem().wstring();
        if (fs::exists(orig_p.parent_path()) &&
            orig_p.parent_path().string().find("ShaderLab\\workspace") == std::string::npos) {
            initial_dir = orig_p.parent_path().wstring();
        }
    }
    if (suggested_stem.empty() && !active_path.empty()) {
        suggested_stem = fs::path(active_path).stem().wstring();
    }
    if (initial_dir.empty()) {
        std::wstring out_w = utf8_to_wide(s_output_folder);
        if (fs::exists(out_w)) {
            initial_dir = out_w;
        } else if (!active_path.empty()) {
            fs::path act_p(active_path);
            if (act_p.parent_path().string().find("ShaderLab\\workspace") == std::string::npos &&
                fs::exists(act_p.parent_path())) {
                initial_dir = act_p.parent_path().wstring();
            }
        }
    }

    if (suggested_stem.empty() || suggested_stem == L"preview") {
        suggested_stem = L"export";
    }

    std::wstring chosen_path;
    if (browse_export_as_dialog(hwnd, suggested_stem, initial_dir, chosen_path)) {
        fs::path chosen_p(chosen_path);
        std::wstring target_dir = chosen_p.parent_path().wstring();
        std::wstring filename = chosen_p.filename().wstring();

        uint32_t flags = 0;
        if (s_auto_converge)      flags |= EXPORT_FLAG_AUTO_CONVERGE;
        if (s_export_wysiwyg)     flags |= EXPORT_FLAG_WYSIWYG;
        if (s_export_embed_depth) {
            if (s_depth_export_format == 0)      flags |= EXPORT_FLAG_EMBED_DEPTH;
            else if (s_depth_export_format == 1) flags |= EXPORT_FLAG_DEPTH_SIDECAR;
            else                                 flags |= (EXPORT_FLAG_EMBED_DEPTH | EXPORT_FLAG_DEPTH_SIDECAR);
        }

        uint32_t settle_cap = s_auto_converge ? 1800 : static_cast<uint32_t>(std::round(s_settle_time_sec * 60.0f));
        if (settle_cap < 30) settle_cap = 30;

        queue_mgr.add_log(L"Export started: " + fs::path(active_path).filename().wstring()
                         + L" -> " + filename);
        queue_mgr.trigger_export(active_path, target_dir, settle_cap, flags, chosen_path);

    }
}

static int s_keybind_capturing = -1;

void on_overlay(reshade::api::effect_runtime *runtime) {
    JobQueueManager &queue_mgr = JobQueueManager::get();
    queue_mgr.update();

    const std::wstring &active_path = queue_mgr.get_active_image_path();
    ExportState state = queue_mgr.get_export_state();
    bool is_exporting = (state == ExportState::Requested ||
                         state == ExportState::Loading ||
                         state == ExportState::Rendering ||
                         state == ExportState::Capturing);

    SharedControlBlock *block = queue_mgr.get_control_block();

    static uint32_t s_last_active_project_version = 0;
    if (block && block->active_project_version != s_last_active_project_version) {
        s_last_active_project_version = block->active_project_version;
        s_current_project_path = block->active_project_path;
    }

    bool want_text = ImGui::GetIO().WantTextInput;
    if (!want_text) {
        if (keybinds::is_pressed(keybinds::Action::SaveProjectAs) ||
            (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))) {
            if (block) {
                block->request_save_project_as++;
            }
        } else if (keybinds::is_pressed(keybinds::Action::SaveProject) ||
                   (ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))) {
            if (block) {
                block->request_save_project++;
            }
        } else if (keybinds::is_pressed(keybinds::Action::ExportImageAs) ||
                   (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_E, false))) {
            if (block) {
                block->request_export_image_as++;
            }
        } else if (keybinds::is_pressed(keybinds::Action::ExportImage) ||
                   (ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_E, false))) {
            if (block) {
                block->request_export_image++;
            }
        } else if (keybinds::is_pressed(keybinds::Action::ToggleBeforeAfter) ||
                   (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_B, false))) {
            before_after_toggle();
        }
    }
    static bool   s_overlay_panning  = false;
    static bool   s_overlay_rotating = false;
    static bool   s_overlay_snapped  = false;
    static float  s_overlay_raw_angle = 0.0f;
    static float  s_overlay_last_drag_ang = 0.0f;
    static bool   s_nudge_left_prev  = false;
    static bool   s_nudge_right_prev = false;

    static constexpr float kSnapStep   = 45.0f;
    static constexpr float kSnapRadius = 4.0f;
    static constexpr float kMinZoom    = 0.25f;
    static constexpr float kMaxZoom    = 32.0f;
    static constexpr float kZoomStep   = 0.12f;  // per wheel notch (fractional)
    static constexpr float kZoomStepFine = 0.04f; // per wheel notch while shift held
    static constexpr float kFineFactor    = 0.25f; // drag scale while shift held
    static constexpr float kDeadZone   = 16.0f;  // pixel²

    if (block) {
        ImGuiIO &io = ImGui::GetIO();

        // sync our local raw angle off the block on first use
        static bool s_angle_initialized = false;
        if (!s_angle_initialized) {
            s_overlay_raw_angle = block->view_raw_angle;
            s_angle_initialized = true;
        }

        // true when the cursor isn't over any imgui window, i.e. hovering the background
        const bool bg_hovered  = !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
        const bool any_active  = ImGui::IsAnyItemActive();
        want_text = io.WantTextInput;
        if (want_text) {
            block->view_interaction_flags |= VIEW_FLAG_TEXT_INPUT;
        } else {
            block->view_interaction_flags &= ~VIEW_FLAG_TEXT_INPUT;
        }
        // poll the fine-tune key via raw key state, same as the host exe, so it still
        // registers when reshade's imgui backend won't report it
        const uint32_t fine_vk  = keybinds::fine_tune_vk();
        const bool     fine     = (GetAsyncKeyState(fine_vk) & 0x8000) != 0;

        // display centre, used as the rotation pivot, matches the host's CompassHud centre-of-screen
        float cx = io.DisplaySize.x * 0.5f;
        float cy = io.DisplaySize.y * 0.5f;

        // check lock states from interaction flags
        const bool lock_zoom   = (block->view_interaction_flags & VIEW_FLAG_LOCK_ZOOM) != 0;
        const bool lock_rotate = (block->view_interaction_flags & VIEW_FLAG_LOCK_ROT) != 0;
        const bool lock_pan    = (block->view_interaction_flags & VIEW_FLAG_LOCK_PAN) != 0;

        // -- zoom (mouse wheel over background) --
        if (bg_hovered && !any_active && io.MouseWheel != 0.0f) {
            if (lock_zoom) {
                block->view_last_zoom_ms = GetTickCount64();
            } else {
                float cur_zoom = block->view_zoom > 0.0f ? block->view_zoom : 1.0f;
                float step = fine ? kZoomStepFine : kZoomStep;
                float factor = 1.0f + step * io.MouseWheel;
                float new_zoom = cur_zoom * factor;
                if (new_zoom < kMinZoom) new_zoom = kMinZoom;
                if (new_zoom > kMaxZoom) new_zoom = kMaxZoom;

                float mouse_dx = io.MousePos.x - cx;
                float mouse_dy = io.MousePos.y - cy;
                float scale_ratio = new_zoom / cur_zoom;
                float adj_x = mouse_dx * (1.0f - 1.0f / scale_ratio) / cur_zoom;
                float adj_y = mouse_dy * (1.0f - 1.0f / scale_ratio) / cur_zoom;

                block->view_zoom = new_zoom;
                block->view_pan[0] += static_cast<int32_t>(std::round(adj_x));
                block->view_pan[1] += static_cast<int32_t>(std::round(adj_y));
                block->view_transform_version++;
                block->view_last_zoom_ms = GetTickCount64();
            }
        }

        bool alt_down = io.KeyAlt;
        static bool s_pan_locked_attempt = false;

        auto &ba_state_mut = before_after_get_state();
        if (!ba_state_mut.is_dragging_pos && !ba_state_mut.is_dragging_rot) {
            ba_state_mut.enabled = (block->view_interaction_flags & VIEW_FLAG_BEFORE_AFTER) != 0;
            ba_state_mut.angle = block->before_after_angle;
            ba_state_mut.split_offset = block->before_after_split;
        }

        before_after_handle_input(bg_hovered, any_active, fine);
        const auto &ba_state = before_after_get_state();
        if (ba_state.enabled) {
            block->before_after_angle = ba_state.angle;
            block->before_after_split = ba_state.split_offset;
        }

        if (!lock_pan && !ba_state.enabled && io.MouseDown[0] && !s_overlay_rotating && !ba_state.is_dragging_pos) {
            s_pan_locked_attempt = false;
            if (!s_overlay_panning) {
                if (bg_hovered && !any_active && !alt_down)
                    s_overlay_panning = true;
            }
            if (s_overlay_panning && !alt_down) {
                float dx = io.MouseDelta.x;
                float dy = io.MouseDelta.y;
                if (fine) { dx *= kFineFactor; dy *= kFineFactor; }
                if (dx != 0.0f || dy != 0.0f) {
                    float rad = block->view_angle * (3.14159265f / 180.0f);
                    float cos_a = std::cos(rad);
                    float sin_a = std::sin(rad);
                    float zoom  = block->view_zoom > 0.0f ? block->view_zoom : 1.0f;
                    float rot_dx = (dx * cos_a + dy * sin_a) / zoom;
                    float rot_dy = (-dx * sin_a + dy * cos_a) / zoom;
                    block->view_pan[0] -= static_cast<int32_t>(std::round(rot_dx));
                    block->view_pan[1] -= static_cast<int32_t>(std::round(rot_dy));
                    block->view_transform_version++;
                }
            }
        } else {
            if (lock_pan && io.MouseDown[0] && bg_hovered && !any_active && !alt_down) {
                s_pan_locked_attempt = true;
            } else {
                s_pan_locked_attempt = false;
            }
            s_overlay_panning = false;
        }

        bool rotate_btn = io.MouseDown[1] || (io.MouseDown[0] && alt_down);

        if (!lock_rotate && rotate_btn && !s_overlay_panning && !ba_state.enabled) {
            if (!s_overlay_rotating) {
                if (bg_hovered && !any_active) {
                    s_overlay_rotating = true;
                    float dx = io.MousePos.x - cx;
                    float dy = io.MousePos.y - cy;
                    s_overlay_last_drag_ang = std::atan2(dx, -dy) * (180.0f / 3.14159265f);
                    if (s_overlay_last_drag_ang < 0.0f) s_overlay_last_drag_ang += 360.0f;
                }
            }
            if (s_overlay_rotating) {
                float dx = io.MousePos.x - cx;
                float dy = io.MousePos.y - cy;
                if (dx * dx + dy * dy > kDeadZone) {
                    float cur_ang = std::atan2(dx, -dy) * (180.0f / 3.14159265f);
                    if (cur_ang < 0.0f) cur_ang += 360.0f;
                    float delta = cur_ang - s_overlay_last_drag_ang;
                    if (delta >  180.0f) delta -= 360.0f;
                    else if (delta < -180.0f) delta += 360.0f;
                    s_overlay_last_drag_ang = cur_ang;
                    if (fine) delta *= kFineFactor;
                    s_overlay_raw_angle += delta;
                    s_overlay_raw_angle = std::fmod(s_overlay_raw_angle, 360.0f);
                    if (s_overlay_raw_angle < 0.0f) s_overlay_raw_angle += 360.0f;

                    float snapped = s_overlay_raw_angle;
                    bool  is_snapped = false;
                    if (!fine) {
                        float r = std::fmod(s_overlay_raw_angle + 0.5f * kSnapStep, kSnapStep) - 0.5f * kSnapStep;
                        if (std::abs(r) < kSnapRadius) {
                            snapped = s_overlay_raw_angle - r;
                            is_snapped = true;
                        }
                    }
                    s_overlay_snapped = is_snapped;
                    block->view_angle     = snapped;
                    block->view_raw_angle = s_overlay_raw_angle;
                    block->view_transform_version++;
                    block->view_last_rotate_ms = GetTickCount64();
                }
            }
        } else {
            if (lock_rotate && rotate_btn && bg_hovered && !any_active) {
                block->view_last_rotate_ms = GetTickCount64();
            }
            s_overlay_rotating = false;
            s_overlay_snapped = false;
        }

        if (!want_text && !any_active && s_keybind_capturing == -1) {
            if (keybinds::is_pressed(keybinds::Action::ResetRotation)) {
                s_overlay_raw_angle = 0.0f;
                block->view_angle     = 0.0f;
                block->view_raw_angle = 0.0f;
                block->view_transform_version++;
            }
            if (keybinds::is_pressed(keybinds::Action::ResetZoomPan)) {
                block->view_zoom   = 1.0f;
                block->view_pan[0] = 0;
                block->view_pan[1] = 0;
                block->view_transform_version++;
            }
            bool nudge_left_down  = (GetAsyncKeyState(VK_LEFT)  & 0x8000) != 0;
            bool nudge_right_down = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
            bool nudge_left  = nudge_left_down  && !s_nudge_left_prev;
            bool nudge_right = nudge_right_down && !s_nudge_right_prev;
            s_nudge_left_prev  = nudge_left_down;
            s_nudge_right_prev = nudge_right_down;

            if (nudge_left && !lock_pan) {
                float screen_step = fine ? 2.0f : 15.0f;
                float rad = block->view_angle * (3.14159265f / 180.0f);
                float cos_a = std::cos(rad);
                float sin_a = std::sin(rad);
                float rot_dx = (-screen_step) * cos_a;
                float rot_dy = -(-screen_step) * sin_a;
                float zoom = block->view_zoom > 0.0f ? block->view_zoom : 1.0f;
                block->view_pan[0] -= static_cast<int32_t>(std::round(rot_dx / zoom));
                block->view_pan[1] -= static_cast<int32_t>(std::round(rot_dy / zoom));
                block->view_transform_version++;
            }
            if (nudge_right && !lock_pan) {
                float screen_step = fine ? 2.0f : 15.0f;
                float rad = block->view_angle * (3.14159265f / 180.0f);
                float cos_a = std::cos(rad);
                float sin_a = std::sin(rad);
                float rot_dx = screen_step * cos_a;
                float rot_dy = -screen_step * sin_a;
                float zoom = block->view_zoom > 0.0f ? block->view_zoom : 1.0f;
                block->view_pan[0] -= static_cast<int32_t>(std::round(rot_dx / zoom));
                block->view_pan[1] -= static_cast<int32_t>(std::round(rot_dy / zoom));
                block->view_transform_version++;
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && bg_hovered) {
                s_overlay_raw_angle   = 0.0f;
                block->view_angle     = 0.0f;
                block->view_raw_angle = 0.0f;
                block->view_zoom      = 1.0f;
                block->view_pan[0]    = 0;
                block->view_pan[1]    = 0;
                block->view_transform_version++;
            }

            uint64_t now_lock_ms = GetTickCount64();
            if (now_lock_ms - block->view_last_lock_ms >= 200) {
                if (keybinds::is_pressed(keybinds::Action::LockPan)) {
                    block->view_interaction_flags ^= VIEW_FLAG_LOCK_PAN;
                    bool locked = (block->view_interaction_flags & VIEW_FLAG_LOCK_PAN) != 0;
                    if (locked) s_overlay_panning = false;
                    char bLock[32];
                    keybinds::describe(keybinds::Action::LockPan, bLock, sizeof(bLock));
                    wchar_t wLock[32];
                    MultiByteToWideChar(CP_UTF8, 0, bLock, -1, wLock, 32);
                    wchar_t msg[64];
                    swprintf_s(msg, locked ? L"Pan locked (%s)" : L"Pan unlocked (%s)", wLock);
                    queue_mgr.add_log(msg);
                    block->view_last_lock_ms = now_lock_ms;
                    block->view_transform_version++;
                } else if (keybinds::is_pressed(keybinds::Action::LockZoom)) {
                    block->view_interaction_flags ^= VIEW_FLAG_LOCK_ZOOM;
                    bool locked = (block->view_interaction_flags & VIEW_FLAG_LOCK_ZOOM) != 0;
                    char bLock[32];
                    keybinds::describe(keybinds::Action::LockZoom, bLock, sizeof(bLock));
                    wchar_t wLock[32];
                    MultiByteToWideChar(CP_UTF8, 0, bLock, -1, wLock, 32);
                    wchar_t msg[64];
                    swprintf_s(msg, locked ? L"Zoom locked (%s)" : L"Zoom unlocked (%s)", wLock);
                    queue_mgr.add_log(msg);
                    block->view_last_lock_ms = now_lock_ms;
                    block->view_transform_version++;
                } else if (keybinds::is_pressed(keybinds::Action::LockRotate)) {
                    block->view_interaction_flags ^= VIEW_FLAG_LOCK_ROT;
                    bool locked = (block->view_interaction_flags & VIEW_FLAG_LOCK_ROT) != 0;
                    if (locked) s_overlay_rotating = false;
                    char bLock[32];
                    keybinds::describe(keybinds::Action::LockRotate, bLock, sizeof(bLock));
                    wchar_t wLock[32];
                    MultiByteToWideChar(CP_UTF8, 0, bLock, -1, wLock, 32);
                    wchar_t msg[64];
                    swprintf_s(msg, locked ? L"Rotation locked (%s)" : L"Rotation unlocked (%s)", wLock);
                    queue_mgr.add_log(msg);
                    block->view_last_lock_ms = now_lock_ms;
                    block->view_transform_version++;
                } else if (keybinds::is_pressed(keybinds::Action::LockView)) {
                    bool any_unlocked = ((block->view_interaction_flags & VIEW_FLAG_LOCK_ZOOM) == 0) ||
                                        ((block->view_interaction_flags & VIEW_FLAG_LOCK_ROT) == 0) ||
                                        ((block->view_interaction_flags & VIEW_FLAG_LOCK_PAN) == 0);
                    char bLock[32];
                    keybinds::describe(keybinds::Action::LockView, bLock, sizeof(bLock));
                    wchar_t wLock[32];
                    MultiByteToWideChar(CP_UTF8, 0, bLock, -1, wLock, 32);
                    wchar_t msg[64];
                    if (any_unlocked) {
                        block->view_interaction_flags |= (VIEW_FLAG_LOCK_ZOOM | VIEW_FLAG_LOCK_ROT | VIEW_FLAG_LOCK_PAN);
                        s_overlay_panning = false;
                        s_overlay_rotating = false;
                        swprintf_s(msg, L"View locked (Pan, Zoom, & Rotate) (%s)", wLock);
                        queue_mgr.add_log(msg);
                    } else {
                        block->view_interaction_flags &= ~(VIEW_FLAG_LOCK_ZOOM | VIEW_FLAG_LOCK_ROT | VIEW_FLAG_LOCK_PAN);
                        swprintf_s(msg, L"View unlocked (%s)", wLock);
                        queue_mgr.add_log(msg);
                    }
                    block->view_last_lock_ms = now_lock_ms;
                    block->view_transform_version++;
                } else if (keybinds::is_pressed(keybinds::Action::ToggleBeforeAfter)) {
                    before_after_toggle();
                    if (before_after_get_state().enabled) {
                        block->view_interaction_flags |= VIEW_FLAG_BEFORE_AFTER;
                    } else {
                        block->view_interaction_flags &= ~VIEW_FLAG_BEFORE_AFTER;
                    }
                    block->before_after_angle = before_after_get_state().angle;
                    block->before_after_split = before_after_get_state().split_offset;
                    block->view_transform_version++;
                }
            }

        }

        static bool s_f11_prev = false;
        bool f11_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        bool f11_pressed = f11_down && !s_f11_prev;
        s_f11_prev = f11_down;

        if (!want_text && s_keybind_capturing == -1 &&
            (f11_pressed || keybinds::is_pressed(keybinds::Action::ToggleFullscreen) || ImGui::IsKeyPressed(ImGuiKey_F11, false))) {
            static uint64_t s_last_fs_req_ms = 0;
            uint64_t now_ms = GetTickCount64();
            if (now_ms - s_last_fs_req_ms > 300) {
                s_last_fs_req_ms = now_ms;
                block->request_toggle_fullscreen++;
            }
        }

        uint32_t preserved_flags = block->view_interaction_flags & (VIEW_FLAG_LOCK_ZOOM | VIEW_FLAG_LOCK_ROT | VIEW_FLAG_LOCK_PAN | VIEW_FLAG_DEPTH_PEEK | VIEW_FLAG_BEFORE_AFTER | VIEW_FLAG_BEFORE_AFTER_DRAG);
        if (want_text) {
            preserved_flags |= VIEW_FLAG_TEXT_INPUT;
        } else {
            preserved_flags &= ~VIEW_FLAG_TEXT_INPUT;
        }
        uint32_t inter_flags = preserved_flags;
        if (ba_state.enabled) {
            inter_flags |= VIEW_FLAG_BEFORE_AFTER;
        } else {
            inter_flags &= ~VIEW_FLAG_BEFORE_AFTER;
        }
        if (ba_state.is_dragging_pos) {
            inter_flags |= VIEW_FLAG_BEFORE_AFTER_DRAG;
        } else {
            inter_flags &= ~VIEW_FLAG_BEFORE_AFTER_DRAG;
        }
        if (s_overlay_panning)    inter_flags |= VIEW_FLAG_PANNING;
        if (s_overlay_rotating)   inter_flags |= VIEW_FLAG_ROTATING;
        if (s_overlay_snapped)    inter_flags |= VIEW_FLAG_SNAPPED;
        if (fine)                 inter_flags |= VIEW_FLAG_FINE;
        if (s_pan_locked_attempt) inter_flags |= VIEW_FLAG_PAN_LOCKED_ATTEMPT;
        if (inter_flags != block->view_interaction_flags) {
            uint32_t old_flags = block->view_interaction_flags;
            block->view_interaction_flags = inter_flags;
            constexpr uint32_t kTransformMask = VIEW_FLAG_ROTATING | VIEW_FLAG_PANNING |
                                                VIEW_FLAG_LOCK_ZOOM | VIEW_FLAG_LOCK_ROT | VIEW_FLAG_LOCK_PAN |
                                                VIEW_FLAG_SNAPPED | VIEW_FLAG_FINE | VIEW_FLAG_PAN_LOCKED_ATTEMPT;
            if ((inter_flags & kTransformMask) != (old_flags & kTransformMask)) {
                block->view_transform_version++;
            }
        }

        if (!s_overlay_rotating) {
            s_overlay_raw_angle = block->view_raw_angle;
        }
    }

    if (block) {
        static uint32_t s_last_preview_counter = 0;
        static uint32_t s_last_depth_version = 0;
        static uint32_t s_prev_depth_valid = 0;
        static bool s_overlay_first_frame = true;

        if (s_overlay_first_frame) {
            s_last_preview_counter = block->preview_counter;
            s_last_depth_version = block->depth_version;
            s_prev_depth_valid = block->depth_valid;
            s_overlay_first_frame = false;
        }

        if (block->preview_counter != s_last_preview_counter) {
            s_last_preview_counter = block->preview_counter;
            s_last_depth_version = block->depth_version;
            s_prev_depth_valid = block->depth_valid;
            if (wcslen(block->preview_path) > 0) {
                queue_mgr.add_log(L"Image loaded: " + std::wstring(block->preview_path));
                if (block->depth_valid) {
                    wchar_t dbuf[128];
                    swprintf_s(dbuf, L"Depth buffer active (%ux%u, Far=%.0f)", block->depth_width, block->depth_height, block->depth_far_plane);
                    queue_mgr.add_log(dbuf);
                } else {
                    queue_mgr.add_log(L"Depth buffer: not detected");
                }
            }
        }

        if (block->depth_version != s_last_depth_version) {
            s_last_depth_version = block->depth_version;
            if (wcslen(block->preview_path) > 0) {
                if (block->depth_valid) {
                    wchar_t dbuf[128];
                    swprintf_s(dbuf, L"Depth buffer active (%ux%u, Far=%.0f)", block->depth_width, block->depth_height, block->depth_far_plane);
                    queue_mgr.add_log(dbuf);
                } else if (s_prev_depth_valid != 0) {
                    queue_mgr.add_log(L"Depth buffer detached");
                }
            }
            s_prev_depth_valid = block->depth_valid;
        }
    }

    ImGui::BeginTabBar("ShaderLabTabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll);
    if (ImGui::BeginTabItem("Main")) {
        // image drop zone
        ImGui::Spacing();
        std::string fname = active_path.empty() ? "" : fs::path(active_path).filename().string();

        float drop_avail = ImGui::GetContentRegionAvail().x;
        const char *drop_txt = (drop_avail < 250.0f) ? "Click to replace image" : "Click or drop to replace image";
        if (ImGui::Button(drop_txt, ImVec2(-1, 42))) {
            std::wstring picked;
            if (browse_image_dialog(picked)) {
                queue_mgr.set_preview_image(picked);
                queue_mgr.add_log(L"Image loaded: " + fs::path(picked).filename().wstring());
            }
        }

        if (fname.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextWrapped("Drop an image here or click button above. Supports PNG, JPG, BMP, TGA, HDR.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // output folder
        ImGui::TextUnformatted("Output Folder:");
        const float change_btn_w = 60.0f;
        const float open_btn_w   = 50.0f;
        const float spacing      = ImGui::GetStyle().ItemSpacing.x;
        const float total_btn_w  = change_btn_w + open_btn_w + spacing;

        ImGui::SetNextItemWidth((std::max)(ImGui::GetContentRegionAvail().x - total_btn_w, 80.0f));
        if (ImGui::InputText("##outdir", s_output_folder, sizeof(s_output_folder))) {
        }
        ImGui::SameLine();
        if (ImGui::Button("Change", ImVec2(change_btn_w, 0))) {
            HWND hwnd = runtime ? static_cast<HWND>(runtime->get_hwnd()) : GetForegroundWindow();
            std::wstring initial = utf8_to_wide(s_output_folder);
            std::wstring picked_dir;
            if (browse_folder_dialog(hwnd, initial, picked_dir)) {
                std::string picked_u8 = wide_to_utf8_ui(picked_dir.c_str());
                strncpy_s(s_output_folder, sizeof(s_output_folder), picked_u8.c_str(), _TRUNCATE);
                queue_mgr.add_log(L"Output folder changed: " + picked_dir);
                strncpy_s(s_prev_output_folder, s_output_folder, sizeof(s_prev_output_folder));
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Browse to change the output folder.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Open", ImVec2(open_btn_w, 0))) {
            std::wstring out_w = utf8_to_wide(s_output_folder);
            if (out_w.empty()) out_w = L"ShaderLab Exported";
            fs::path p(out_w);
            if (!fs::exists(p)) {
                std::error_code ec;
                fs::create_directories(p, ec);
            }
            ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOW);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Open current output folder in Windows File Explorer.");
        }

        if (ImGui::IsItemDeactivatedAfterEdit() || strcmp(s_output_folder, s_prev_output_folder) != 0) {
            if (strcmp(s_output_folder, s_prev_output_folder) != 0) {
                queue_mgr.add_log(L"Output folder set to: " + utf8_to_wide(s_output_folder));
                strncpy_s(s_prev_output_folder, s_output_folder, sizeof(s_prev_output_folder));
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // export button & progress
        static double s_export_done_timestamp = -100.0;
        static ExportState s_prev_export_state = ExportState::Idle;
        if (s_prev_export_state != ExportState::Done && state == ExportState::Done) {
            s_export_done_timestamp = ImGui::GetTime();
        }
        s_prev_export_state = state;

        bool export_disabled = active_path.empty() || is_exporting;
        float export_progress = -1.0f;
        const char *exp_label = "Export Image";

        double now = ImGui::GetTime();
        bool show_complete = (!is_exporting && (now - s_export_done_timestamp) < 1.8);

        if (is_exporting) {
            if (state == ExportState::Rendering && block) {
                if (block->export_converged != 0) {
                    export_progress = 1.0f;
                } else if (s_auto_converge) {
                    export_progress = -1.0f;
                } else {
                    uint32_t max_frames = static_cast<uint32_t>(std::round(s_settle_time_sec * 60.0f));
                    if (max_frames < 30) max_frames = 30;
                    export_progress = std::clamp(static_cast<float>(block->export_frame_index) / static_cast<float>(max_frames), 0.0f, 0.95f);
                }
            } else if (state == ExportState::Capturing) {
                export_progress = 1.0f;
            } else {
                export_progress = -1.0f;
            }
            exp_label = "Exporting...";
        } else if (show_complete) {
            export_progress = 1.0f;
            exp_label = "Export Complete!";
        }

        if (DrawProgressButton("##ExportImageBtn", exp_label, export_progress, (is_exporting || show_complete), export_disabled, ImVec2(-1, 38))) {
            trigger_export_workflow(runtime);
        }

        // live progress line during export
        if (is_exporting) {
            if (state == ExportState::Rendering && block) {
                uint32_t f_idx = block->export_frame_index;
                float delta = block->export_last_delta;
                uint32_t max_frames = static_cast<uint32_t>(std::round(s_settle_time_sec * 60.0f));
                if (max_frames < 30) max_frames = 30;

                ImGui::PushTextWrapPos(0.0f);
                if (block->export_converged) {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f),
                        "Flushing... (converged early, delta=%.4f)", delta);
                } else if (block->effects_compiling != 0) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                        "Compiling ReShade effects on resized canvas...");
                } else if (s_auto_converge) {
                    float cur_sec = static_cast<float>(f_idx) / 60.0f;
                    ImGui::Text("Settling... %.1fs (frame %u, Smart Settle), delta=%.4f", cur_sec, f_idx, delta);
                } else {
                    float cur_sec = static_cast<float>(f_idx) / 60.0f;
                    ImGui::Text("Settling... %.1fs / %.1fs (frame %u/%u)", cur_sec, s_settle_time_sec, f_idx, max_frames);
                }
            } else if (state == ExportState::Capturing) {
                if (block && block->export_converged) {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "Converged! Capturing image...");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Cap reached. Capturing image...");
                }
            } else {
                ImGui::TextDisabled("Preparing export pipeline...");
            }
            ImGui::PopTextWrapPos();
        } else if (block && wcslen(block->export_status) > 0) {
            std::string status_u8 = wide_to_utf8_ui(block->export_status);
            if (block->export_error == IPC_OK) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
                ImGui::TextWrapped("%s", status_u8.c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImGui::TextWrapped("%s", status_u8.c_str());
                ImGui::PopStyleColor();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // controls help
        if (ImGui::CollapsingHeader("Viewport Controls & Shortcuts")) {
            char bFine[64], bNudgeL[64], bNudgeR[64], bResetZoom[64], bResetRot[64], bUndo[64], bRedo[64];
            char bLockP[64], bLockZ[64], bLockR[64], bLockV[64], bFS[64];
            keybinds::describe(keybinds::Action::FineTune, bFine, sizeof(bFine));
            keybinds::describe(keybinds::Action::NudgeLeft, bNudgeL, sizeof(bNudgeL));
            keybinds::describe(keybinds::Action::NudgeRight, bNudgeR, sizeof(bNudgeR));
            keybinds::describe(keybinds::Action::ResetZoomPan, bResetZoom, sizeof(bResetZoom));
            keybinds::describe(keybinds::Action::ResetRotation, bResetRot, sizeof(bResetRot));
            keybinds::describe(keybinds::Action::Undo, bUndo, sizeof(bUndo));
            keybinds::describe(keybinds::Action::Redo, bRedo, sizeof(bRedo));
            keybinds::describe(keybinds::Action::LockPan, bLockP, sizeof(bLockP));
            keybinds::describe(keybinds::Action::LockZoom, bLockZ, sizeof(bLockZ));
            keybinds::describe(keybinds::Action::LockRotate, bLockR, sizeof(bLockR));
            keybinds::describe(keybinds::Action::LockView, bLockV, sizeof(bLockV));
            keybinds::describe(keybinds::Action::ToggleFullscreen, bFS, sizeof(bFS));

            BulletTextWrapped("Mouse wheel: Zoom (0.25x - 32x)");
            BulletTextWrapped("Left-Drag: Pan the image");
            BulletTextWrapped("Right-Drag or Alt+Drag: Rotate (snaps 45 deg)");
            BulletTextWrapped("Hold %s: Fine-tune (slow pan & zoom)", bFine);
            BulletTextWrapped("%s / %s: Pan left / right", bNudgeL, bNudgeR);
            BulletTextWrapped("%s: Reset zoom & pan", bResetZoom);
            BulletTextWrapped("%s: Reset rotation", bResetRot);
            BulletTextWrapped("%s: Borderless Fullscreen", bFS);
            BulletTextWrapped("Double-Click: Reset all");
            ImGui::Separator();
            ImGui::TextDisabled("View Locks:");
            BulletTextWrapped("%s: Lock Pan   |   %s: Lock Zoom", bLockP, bLockZ);
            BulletTextWrapped("%s: Lock Rotate|   %s: Lock View (All)", bLockR, bLockV);
            ImGui::Separator();
            ImGui::TextDisabled("Shader Edits Undo / Redo:");
            BulletTextWrapped("%s: Undo  |  %s: Redo  |  Ctrl+Shift+Z: Redo", bUndo, bRedo);
            char bSave[64], bSaveAs[64], bExp[64], bExpAs[64];
            keybinds::describe(keybinds::Action::SaveProject, bSave, sizeof(bSave));
            keybinds::describe(keybinds::Action::SaveProjectAs, bSaveAs, sizeof(bSaveAs));
            keybinds::describe(keybinds::Action::ExportImage, bExp, sizeof(bExp));
            keybinds::describe(keybinds::Action::ExportImageAs, bExpAs, sizeof(bExpAs));
            ImGui::Separator();
            ImGui::TextDisabled("Export Operations:");
            BulletTextWrapped("%s: Export Image", bExp);
            BulletTextWrapped("%s: Export Image As...", bExpAs);
            ImGui::Separator();
            ImGui::TextDisabled("Project Management:");
            BulletTextWrapped("%s: Save Project", bSave);
            BulletTextWrapped("%s: Save Project As...", bSaveAs);
            BulletTextWrapped("Rebind any of these in Keybinds tab.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // footer
        float win_w = ImGui::GetWindowWidth();
        ImGui::TextDisabled("ShaderLab v1.2.1  -  by NotRaySt");
        if (win_w >= 540.0f) {
            ImGui::SameLine();
        } else {
            ImGui::Spacing();
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 3.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f); // squircle
        if (ImGui::Button("GitHub")) {
            ShellExecuteW(nullptr, L"open", L"https://github.com/NotRayST/ShaderLab", nullptr, nullptr, SW_SHOW);
        }
        ImGui::SameLine();
        if (ImGui::Button("Support me on Patreon")) {
            ShellExecuteW(nullptr, L"open", L"https://www.patreon.com/cw/RayST", nullptr, nullptr, SW_SHOW);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("https://www.patreon.com/cw/RayST");
        }
        ImGui::PopStyleVar(2); // FramePadding, FrameRounding

        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Depth")) {
        try {
            DepthManager &depth_mgr = DepthManager::get();
            bool has_depth = (block && block->depth_valid != 0);

        // depth estimation
        ImGui::Spacing();
        ImGui::TextUnformatted("Machine Learning Depth Estimation");
        
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (has_depth) {
            ImGui::TextWrapped("Depth buffer is active on this image. You can re-generate it using machine learning below.");
        } else {
            ImGui::TextWrapped("No depth buffer detected. Generate a high-fidelity 3D depth buffer using machine learning.");
        }
        ImGui::PopStyleColor();

        static float s_ai_far_plane = 50.0f;
        static int s_ai_model_idx = 0;
        static int s_ai_input_size = 1008;
        static uint32_t s_last_img_w = 0, s_last_img_h = 0;
        static bool s_res_user_set = false;
        static float s_ai_gamma = 2.0f;
        static float s_ai_near_threshold = 0.0f;
        static float s_ai_sky_threshold = 0.0f;
        static bool s_ai_edge_refine = false;
        static bool s_ai_invert = false;

        const char *s_model_names[] = {
            "Large (~1.3GB) - 335M Quality",
            "Base (~390MB) - 97M Balanced",
            "Small (~100MB) - 25M Fast"
        };
        const char *encoders[] = { "vitl", "vitb", "vits" };

        int max_res = 2016;
        int default_res = 1008;
        if (block && block->view_image_width > 0 && block->view_image_height > 0) {
            int img_max = (std::max)(block->view_image_width, block->view_image_height);
            max_res = (img_max / 14) * 14;
            if (max_res < 392) max_res = 392;

            default_res = ((img_max / 2) / 14) * 14;
            if (default_res < 392) default_res = 392;
            if (default_res > max_res) default_res = max_res;

            if (block->view_image_width != s_last_img_w || block->view_image_height != s_last_img_h) {
                s_last_img_w = block->view_image_width;
                s_last_img_h = block->view_image_height;
                if (!s_res_user_set) {
                    s_ai_input_size = default_res;
                }
            }
        }
        if (s_ai_input_size > max_res) s_ai_input_size = max_res;
        if (s_ai_input_size < 392) s_ai_input_size = 392;

        bool ai_running = depth_mgr.is_ai_running();
        bool downloading = depth_mgr.is_downloading();
        bool model_present = depth_mgr.is_model_present(encoders[s_ai_model_idx]);

        std::string btn_label;
        float btn_progress = -1.0f;
        bool is_busy = false;
        float avail_w = ImGui::GetContentRegionAvail().x;

        if (downloading) {
            btn_progress = depth_mgr.get_download_progress();
            std::string status_txt = depth_mgr.get_download_status_text();
            btn_label = status_txt.empty() ? "Downloading Model..." : ("Downloading: " + status_txt);
            is_busy = true;
        } else if (ai_running) {
            float p = depth_mgr.get_ai_progress();
            btn_progress = (p >= 0.0f) ? p : -1.0f;
            btn_label = (avail_w < 260.0f) ? "Generating Depth..." : "Generating Depth (AI)...";
            is_busy = true;
        } else if (!model_present) {
            if (avail_w < 280.0f) {
                btn_label = (s_ai_model_idx == 0) ? "Download & Generate (1.3GB)"
                          : (s_ai_model_idx == 1) ? "Download & Generate (390MB)"
                          : "Download & Generate (100MB)";
            } else {
                btn_label = (s_ai_model_idx == 0) ? "Download & Generate Depth (1.3GB)"
                          : (s_ai_model_idx == 1) ? "Download & Generate Depth (390MB)"
                          : "Download & Generate Depth (100MB)";
            }
            is_busy = false;
        } else {
            btn_label = (avail_w < 260.0f) ? "Generate Depth" : "Generate Depth Buffer";
            is_busy = false;
        }

        ImGui::Spacing();
        bool btn_disabled = active_path.empty() || is_busy;
        if (DrawProgressButton("##GenDepthBtn", btn_label.c_str(), btn_progress, is_busy, btn_disabled, ImVec2(-1, 38))) {
            if (!model_present) {
                depth_mgr.trigger_model_download(encoders[s_ai_model_idx]);
            } else {
                depth_mgr.trigger_ai_depth(
                    active_path,
                    s_ai_far_plane,
                    false,
                    encoders[s_ai_model_idx],
                    s_ai_input_size,
                    s_ai_gamma,
                    s_ai_near_threshold,
                    s_ai_sky_threshold,
                    s_ai_edge_refine,
                    s_ai_invert
                );
            }
        }

        if (ai_running) {
            std::string status = depth_mgr.get_ai_status();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.8f, 1.0f, 1.0f));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextWrapped("Processing: %s", status.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        } else {
            std::string err = depth_mgr.get_ai_last_error();
            if (!err.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextWrapped("Error: %s", err.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
        }

        if (ImGui::TreeNode("Depth Estimation Settings")) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Model Architecture & Quality:");
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##ModelSize", &s_ai_model_idx, s_model_names, IM_ARRAYSIZE(s_model_names));

            if (!model_present && !downloading) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextWrapped("Model checkpoint not detected locally. Click button above to download.");
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("Inference Resolution: %d px", s_ai_input_size);
            ImGui::PopTextWrapPos();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##InferenceResSlider", &s_ai_input_size, 392, max_res, "%d px", ImGuiSliderFlags_None)) {
                s_res_user_set = true;
                int rem = s_ai_input_size % 14;
                if (rem != 0) {
                    if (rem >= 7) s_ai_input_size += (14 - rem);
                    else s_ai_input_size -= rem;
                }
                if (s_ai_input_size < 392) s_ai_input_size = 392;
                if (s_ai_input_size > max_res) s_ai_input_size = max_res;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Inference resolution passed to Vision Transformer (snapped in steps of 14px).\n"
                                  "Min: 392px (Fast)\n"
                                  "Default: %dpx (Half-resolution for fast inference)\n"
                                  "Max: %dpx (Native image dimension cap)", default_res, max_res);
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Depth Contrast Curve (Gamma):");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##AiGamma", &s_ai_gamma, 0.4f, 2.5f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("< 1.0 expands background/landscape depth separation\n= 1.0 linear depth (default)\n> 1.0 increases foreground character focus & contrast");
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Foreground / Near Threshold:");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##AiNear", &s_ai_near_threshold, 0.0f, 10.0f, s_ai_near_threshold == 0.0f ? "Disabled (0.00)" : "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Clamps all depth values below this threshold directly to zero (camera near plane).");
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Sky & Infinity Threshold:");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##AiSky", &s_ai_sky_threshold, 0.0f, 0.99f, s_ai_sky_threshold == 0.0f ? "Disabled (0.00)" : "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Clamps distant sky/background beyond this depth to pure infinity (1.0), preventing horizon lighting artifacts in shaders.");
            }

            ImGui::Spacing();
            ImGui::Checkbox("Snap Contours to Edges (Bilateral)", &s_ai_edge_refine);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Uses an edge-preserving filter to align depth silhouettes tightly to the RGB image contours.");
            }

            ImGui::Spacing();
            ImGui::Checkbox("Invert Polarity (0 = Far, 1 = Near)", &s_ai_invert);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Inverts depth values. Leave unchecked for standard ReShade shaders (0 = Near, 1 = Far).");
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Virtual Far Plane:");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##VirtualFarPlane", &s_ai_far_plane, 1.0f, 10000.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Virtual depth far plane distance calibration (Default: 50.0).\nRange: 1.0 - 10000.0\nTip: Ctrl+Click to type any custom value.");
            }

            ImGui::Spacing();
            if (ImGui::Button("Reset Depth Defaults", ImVec2(-1, 26))) {
                s_ai_model_idx = 0;
                s_ai_input_size = default_res;
                s_res_user_set = false;
                s_ai_gamma = 2.0f;
                s_ai_near_threshold = 0.0f;
                s_ai_sky_threshold = 0.0f;
                s_ai_edge_refine = false;
                s_ai_invert = false;
                s_ai_far_plane = 50.0f;
            }

            ImGui::TreePop();
        }


        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // attach depth from screenshot
        ImGui::TextUnformatted("Attach Depth from Screenshot / B&W Image");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("Pair a depth screenshot (from DisplayDepth.fx or custom render) with the active image.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        std::string depth_path_utf8 = wide_to_utf8_ui(s_selected_depth_path);
        std::string depth_display_name = depth_path_utf8.empty() ? "[ Click to Browse Depth Screenshot ]" : fs::path(s_selected_depth_path).filename().string();

        if (ImGui::Button(depth_display_name.c_str(), ImVec2(-1, 35))) {
            std::wstring picked;
            if (browse_image_dialog(picked)) {
                wcsncpy_s(s_selected_depth_path, MAX_PATH, picked.c_str(), _TRUNCATE);
            }
        }

        if (wcslen(s_selected_depth_path) > 0) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("Selected Depth: %s", depth_path_utf8.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Checkbox("Invert Depth (White = Near, Black = Far)", &s_ss_invert);
            ImGui::SliderFloat("Far Plane (F)##SSFarPlane", &s_ss_far_plane, 1.0f, 10000.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Virtual depth far plane distance calibration (Default: 1000.0).\nRange: 1.0 - 10000.0\nTip: Ctrl+Click to type any custom value.");
            }

            static std::string s_attach_err = "";
            ImGui::BeginDisabled(active_path.empty());
            if (ImGui::Button("Attach Depth Map & Reload", ImVec2(-1, 32))) {
                s_attach_err.clear();
                if (!depth_mgr.attach_depth_from_image(active_path, s_selected_depth_path, s_ss_invert, s_ss_far_plane, false, s_attach_err)) {
                    queue_mgr.add_log(L"Attach Depth Error: " + utf8_to_wide(s_attach_err.c_str()));
                } else {
                    s_selected_depth_path[0] = L'\0';
                }
            }
            ImGui::EndDisabled();

            if (!s_attach_err.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", s_attach_err.c_str());
                ImGui::PopTextWrapPos();
            }
            }
        } catch (const std::exception &ex) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Depth tab error: %s", ex.what());
            ImGui::PopTextWrapPos();
        } catch (...) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Depth tab error: unexpected error");
            ImGui::PopTextWrapPos();
        }

        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Settings")) {
        try {
            bool has_depth = (block && block->depth_valid != 0);

        ImGui::Spacing();
        ImGui::TextUnformatted("Export Settings");

        ImGui::Checkbox("Smart Settle", &s_auto_converge);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Automatically detects when temporal shaders, accumulation buffers, and effects have converged and stabilized before capturing.");
        }

        int calculated_frames = static_cast<int>(std::round(s_settle_time_sec * 60.0f));

        ImGui::BeginDisabled(s_auto_converge);
        ImGui::SliderFloat("Settle Time", &s_settle_time_sec, 0.5f, 60.0f, "%.1f s");
        if (s_settle_time_sec < 0.5f) s_settle_time_sec = 0.5f;
        ImGui::EndDisabled();
        
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (s_auto_converge) {
            ImGui::TextWrapped("Smart Settle is active (waits for ReShade shaders to finish loading, then captures automatically as soon as effects settle and stabilize).");
        } else {
            ImGui::TextWrapped("Renders for %.1f seconds (%d frames at 60 FPS) before capturing.", s_settle_time_sec, calculated_frames);
        }
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Checkbox("Apply Rotation & Zoom on Export", &s_export_wysiwyg);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When off, exports the original image with shaders only, ignoring view rotation/zoom/pan.");
        }

        if (block) {
            float angle = block->view_angle;
            float zoom = block->view_zoom > 0.0f ? block->view_zoom : 1.0f;
            int px = block->view_pan[0];
            int py = block->view_pan[1];

            bool is_custom_transform = (std::abs(angle) > 0.01f || std::abs(zoom - 1.0f) > 0.01f || px != 0 || py != 0);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (is_custom_transform) {
                ImGui::TextWrapped("[Current view: %.1f deg, %d%% zoom, pan: (%+d, %+d)]", angle, static_cast<int>(std::round(zoom * 100.0f)), px, py);
            } else {
                ImGui::TextWrapped("[Current view: default 1:1]");
            }
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Checkbox("Export Depth Map with Image", &s_export_embed_depth);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Exports depth data alongside your final processed image.");
        }
        if (s_export_embed_depth) {
            ImGui::Indent();
            ImGui::TextDisabled("Export Format:");
            const char *depth_fmt_items[] = {
                "Embed in PNG (slDp Chunk)",
                "Sidecar File (.sldepth)",
                "Both (PNG Chunk & Sidecar)"
            };
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##DepthExportFormat", &s_depth_export_format, depth_fmt_items, IM_ARRAYSIZE(depth_fmt_items));
            ImGui::Unindent();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Depth Operations");

        ImGui::BeginDisabled(!has_depth || active_path.empty());
        if (ImGui::Button("Remove Depth from Active Image", ImVec2(-1, 30))) {
            std::string err;
            if (DepthManager::get().remove_depth(active_path, err)) {
                queue_mgr.add_log(L"Removed depth from active image");
                if (block && block->active_project_path[0] != L'\0') block->project_dirty = 1;
            }
        }
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("ReShade Preprocessor Definitions");

        float adv_avail = ImGui::GetContentRegionAvail().x;
        const char *reset_defs_label = (adv_avail < 280.0f) ? "Reset Depth Definitions" : "Reset Depth Preprocessor Definitions";
        if (ImGui::Button(reset_defs_label, ImVec2(-1, 30))) {
            runtime->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN", "0");
            runtime->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", "0");
            runtime->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", "0");
            runtime->set_preprocessor_definition("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", "1000.0");
            const char defs_array[] =
                "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0\0"
                "RESHADE_DEPTH_INPUT_IS_REVERSED=0\0"
                "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0\0"
                "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000.0\0";
            reshade::set_config_value(runtime, "GENERAL", "PreprocessorDefinitions", defs_array, sizeof(defs_array));
            queue_mgr.add_log(L"Reset depth preprocessors to defaults (REVERSED=0, UPSIDEDOWN=0, LOG=0, FAR=1000.0)");
            if (block && block->active_project_path[0] != L'\0') block->project_dirty = 1;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Resets ReShade depth preprocessors to ShaderLab defaults:\n"
                              "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0\n"
                              "RESHADE_DEPTH_INPUT_IS_REVERSED=0\n"
                              "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0\n"
                              "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000.0\n\n"
                              "Click this if shaders like RTGI, ReLIGHT, or DisplayDepth are not reading depth correctly.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Effect History (undo/redo)");
        
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("Click an entry to jump.\n%zu undoable, %zu redoable.",
                           undo_history::undo_count(), undo_history::redo_count());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        size_t hsize = undo_history::history_size();
        if (hsize == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("No edits recorded yet.");
            ImGui::PopStyleColor();
        } else {
            ImGui::BeginChild("EffectHistory", ImVec2(0, 160), true, ImGuiWindowFlags_HorizontalScrollbar);
            bool is_at_base = (undo_history::undo_count() == 0);
            if (!is_at_base)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
            if (ImGui::Selectable("[Initial Preset State]", is_at_base)) {
                undo_history::jump_to(runtime, hsize);
            }
            if (!is_at_base)
                ImGui::PopStyleColor();

            for (size_t i = hsize; i-- > 0;) {
                char label[256] = {};
                if (!undo_history::history_label(i, label, sizeof(label)))
                    continue;
                bool undone = undo_history::history_undone(i);
                bool is_selected = (!undone && (i == undo_history::redo_count()));
                if (undone)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(label, is_selected)) {
                    undo_history::jump_to(runtime, i);
                }
                ImGui::PopID();
                if (undone)
                    ImGui::PopStyleColor();
            }
            ImGui::EndChild();
        }

        if (ImGui::Button("Clear History")) {
            undo_history::clear();
        }
        } catch (const std::exception &ex) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Advanced tab error: %s", ex.what());
            ImGui::PopTextWrapPos();
        } catch (...) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Advanced tab error: unexpected error");
            ImGui::PopTextWrapPos();
        }

        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Keybinds")) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Click a binding, then press the key you want. Modifiers (Ctrl/Shift/Alt) are recorded automatically.");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        struct HeldState {
            bool ctrl = false;
            bool shift = false;
            bool alt = false;
            ImGuiKey normal_key = ImGuiKey_None;
            ImGuiKey last_mod_key = ImGuiKey_None;

            bool empty() const {
                return !ctrl && !shift && !alt && (normal_key == ImGuiKey_None);
            }
            int count() const {
                return (ctrl ? 1 : 0) + (shift ? 1 : 0) + (alt ? 1 : 0) + (normal_key != ImGuiKey_None ? 1 : 0);
            }
        };

        int &s_capturing = s_keybind_capturing;
        static int s_capturing_start_frame = 0;
        static HeldState s_peak_chord;
        static bool s_has_peak = false;
        static uint64_t s_release_start_ms = 0;
        static bool s_in_release = false;

        static bool s_prev_ctrl = false;
        static bool s_prev_shift = false;
        static bool s_prev_alt = false;
        static ImGuiKey s_last_mod_down = ImGuiKey_None;

        constexpr int kActionCount = static_cast<int>(keybinds::Action::Count);
        for (int a = 0; a < kActionCount; ++a) {
            keybinds::Action action = static_cast<keybinds::Action>(a);
            ImGui::Text("%s", keybinds::name(action));
            ImGui::SameLine(200.0f);

            if (s_capturing == a) {
                // cancels on Escape or click away (only after activation frame and when no keys held)
                if (ImGui::GetFrameCount() > s_capturing_start_frame + 2) {
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                        s_capturing = -1;
                    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, false) && !s_has_peak) {
                        s_capturing = -1;
                    }
                }

                if (s_capturing == a) {
                    HeldState current;
                    current.ctrl  = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) || ImGui::GetIO().KeyCtrl;
                    current.shift = ((GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0) || ImGui::GetIO().KeyShift;
                    current.alt   = ((GetAsyncKeyState(VK_MENU)    & 0x8000) != 0) || ImGui::GetIO().KeyAlt;

                    // track most recently pressed modifier
                    if (current.ctrl && !s_prev_ctrl)   s_last_mod_down = (GetAsyncKeyState(VK_RCONTROL) & 0x8000) ? ImGuiKey_RightCtrl : ImGuiKey_LeftCtrl;
                    if (current.shift && !s_prev_shift) s_last_mod_down = (GetAsyncKeyState(VK_RSHIFT)   & 0x8000) ? ImGuiKey_RightShift : ImGuiKey_LeftShift;
                    if (current.alt && !s_prev_alt)     s_last_mod_down = (GetAsyncKeyState(VK_RMENU)    & 0x8000) ? ImGuiKey_RightAlt : ImGuiKey_LeftAlt;

                    s_prev_ctrl  = current.ctrl;
                    s_prev_shift = current.shift;
                    s_prev_alt   = current.alt;

                    // find non-modifier key currently held
                    for (int kk = static_cast<int>(ImGuiKey_NamedKey_BEGIN); kk < static_cast<int>(ImGuiKey_NamedKey_END); ++kk) {
                        ImGuiKey k = static_cast<ImGuiKey>(kk);
                        if (k == ImGuiKey_Escape) continue;
                        if (k >= ImGuiKey_MouseLeft && k <= ImGuiKey_MouseMiddle) continue;
                        if (keybinds::is_modifier_key(k)) continue;
                        if (k >= ImGuiKey_ReservedForModCtrl && k <= ImGuiKey_ReservedForModSuper) continue;

                        if (ImGui::IsKeyDown(k)) {
                            current.normal_key = k;
                            break;
                        }
                    }
                    current.last_mod_key = s_last_mod_down;

                    uint64_t now = GetTickCount64();
                    constexpr uint64_t kReleaseBufferMs = 250;

                    int cur_count = current.count();
                    int peak_count = s_has_peak ? s_peak_chord.count() : 0;

                    if (cur_count > 0) {
                        if (cur_count >= peak_count || (current.normal_key != ImGuiKey_None && s_peak_chord.normal_key == ImGuiKey_None)) {
                            // chord reached a new maximum or added a primary non-modifier key
                            s_peak_chord = current;
                            s_has_peak = true;
                            s_in_release = false;
                            s_release_start_ms = 0;
                        } else {
                            // key count decreased
                            if (!s_in_release) {
                                s_in_release = true;
                                s_release_start_ms = now;
                            } else if (now - s_release_start_ms > kReleaseBufferMs) {
                                // user released a key but held remainder past buffer window
                                s_peak_chord = current;
                                s_in_release = false;
                                s_release_start_ms = 0;
                            }
                        }
                    }

                    // live preview
                    int anim_dots = 1 + static_cast<int>(fmod(ImGui::GetTime() * 3.0, 3.0));
                    std::string dots_str(anim_dots, '.');

                    std::string preview;
                    if (!current.empty()) {
                        // physically held keys right now
                        if (current.ctrl)  preview += "Ctrl+";
                        if (current.shift) preview += "Shift+";
                        if (current.alt)   preview += "Alt+";
                        if (current.normal_key != ImGuiKey_None) {
                            preview += ImGui::GetKeyName(current.normal_key);
                        } else {
                            preview += dots_str;
                        }
                    } else if (s_has_peak && !s_peak_chord.empty()) {
                        if (s_peak_chord.ctrl)  preview += "Ctrl+";
                        if (s_peak_chord.shift) preview += "Shift+";
                        if (s_peak_chord.alt)   preview += "Alt+";
                        if (s_peak_chord.normal_key != ImGuiKey_None) {
                            preview += ImGui::GetKeyName(s_peak_chord.normal_key);
                        } else if (s_peak_chord.last_mod_key != ImGuiKey_None) {
                            preview += (keybinds::is_shift_key(s_peak_chord.last_mod_key) ? "Shift" :
                                        keybinds::is_ctrl_key(s_peak_chord.last_mod_key) ? "Ctrl" : "Alt");
                        }
                    } else {
                        preview = "Press a key" + dots_str;
                    }

                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", preview.c_str());

                    // commit when all keys released
                    if (s_has_peak && current.empty() && ImGui::GetFrameCount() > s_capturing_start_frame + 2) {
                        ImGuiKey primary_key = ImGuiKey_None;
                        bool req_ctrl = false;
                        bool req_shift = false;
                        bool req_alt = false;

                        if (s_peak_chord.normal_key != ImGuiKey_None) {
                            primary_key = s_peak_chord.normal_key;
                            req_ctrl  = s_peak_chord.ctrl;
                            req_shift = s_peak_chord.shift;
                            req_alt   = s_peak_chord.alt;
                        } else if (s_peak_chord.count() > 0) {
                            ImGuiKey mod = s_peak_chord.last_mod_key;
                            if (mod == ImGuiKey_None) {
                                if (s_peak_chord.shift) mod = ImGuiKey_LeftShift;
                                else if (s_peak_chord.ctrl) mod = ImGuiKey_LeftCtrl;
                                else if (s_peak_chord.alt) mod = ImGuiKey_LeftAlt;
                            }
                            primary_key = mod;

                            if (keybinds::is_ctrl_key(mod)) {
                                req_shift = s_peak_chord.shift;
                                req_alt   = s_peak_chord.alt;
                            } else if (keybinds::is_shift_key(mod)) {
                                req_ctrl = s_peak_chord.ctrl;
                                req_alt  = s_peak_chord.alt;
                            } else if (keybinds::is_alt_key(mod)) {
                                req_ctrl  = s_peak_chord.ctrl;
                                req_shift = s_peak_chord.shift;
                            }
                        }

                        if (primary_key != ImGuiKey_None) {
                            keybinds::set_key(action, primary_key, req_ctrl, req_shift, req_alt);
                        }

                        s_capturing = -1;
                        s_peak_chord = HeldState{};
                        s_has_peak = false;
                        s_in_release = false;
                        s_release_start_ms = 0;
                        s_prev_ctrl = false;
                        s_prev_shift = false;
                        s_prev_alt = false;
                        s_last_mod_down = ImGuiKey_None;
                    }
                }
            } else {
                char b[96];
                keybinds::describe(action, b, sizeof(b));
                char id[104];
                snprintf(id, sizeof(id), "%s###kb%d", b, a);
                if (ImGui::Button(id, ImVec2(120.0f, 0))) {
                    s_capturing = a;
                    s_capturing_start_frame = ImGui::GetFrameCount();
                    s_peak_chord = HeldState{};
                    s_has_peak = false;
                    s_in_release = false;
                    s_release_start_ms = 0;
                    s_prev_ctrl = false;
                    s_prev_shift = false;
                    s_prev_alt = false;
                    s_last_mod_down = ImGuiKey_None;
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Reset to Defaults")) {
            keybinds::reset_to_defaults();
        }

        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Logs")) {
        const auto &logs = queue_mgr.get_logs();

        ImGui::Spacing();
        static bool s_auto_scroll_logs = true;
        ImGui::Checkbox("Auto-scroll", &s_auto_scroll_logs);
        ImGui::SameLine();
        if (ImGui::Button("Copy Logs")) {
            std::string combined;
            for (const auto &line : logs) {
                combined += wide_to_utf8_ui(line.c_str());
                combined += "\n";
            }
            ImGui::SetClipboardText(combined.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Logs")) {
            queue_mgr.clear_logs();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImVec2 avail = ImGui::GetContentRegionAvail();
        float list_height = (std::max)(avail.y - 30.0f, 150.0f);

        ImGui::BeginChild("FullLogScroll", ImVec2(0, list_height), true, ImGuiWindowFlags_HorizontalScrollbar);
        if (logs.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("No log messages recorded yet.");
            ImGui::PopStyleColor();
        } else {
            for (const auto &line : logs) {
                std::string s = wide_to_utf8_ui(line.c_str());
                ImGui::TextUnformatted(s.c_str());
            }
            if (s_auto_scroll_logs && (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f || ImGui::IsWindowAppearing())) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Total entries: %zu / 1000", logs.size());
        ImGui::PopTextWrapPos();

        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("About")) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.35f, 0.70f, 1.00f, 1.00f), "ShaderLab");
        ImGui::SameLine();
        ImGui::TextDisabled("v1.2.1");

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("Offline image processing and editing tool using reshade shaders.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // personal note card
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.16f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.976f, 0.408f, 0.329f, 0.50f)); 
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 14.0f));

        static float s_about_note_height = 0.0f;
        if (ImGui::BeginChild("AboutNoteCard", ImVec2(0, s_about_note_height), true, ImGuiWindowFlags_NoScrollbar)) {
            float start_y = ImGui::GetCursorPosY();

            ImGui::TextColored(ImVec4(0.976f, 0.408f, 0.329f, 1.0f), "A Personal Note from the Developer");
            ImGui::Spacing();

            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextWrapped(
                "Hi, I'm RaySt, a solo developer and engineering student.\n\n"
                "ShaderLab started as a passion project to bring the artistic power of ReShade to offline images. "
                "Building it into a complete, high-performance tool for artists and virtual photographers, without forcing anyone into expensive creative subscriptions like Adobe's Lightroom, is a massive undertaking that still has a long road ahead.\n\n"
                "Balancing tough engineering studies with the immense amount of time spent reverse-engineering, researching complex features, implementing them, "
                "and tracking down bugs means constantly taking time away from family, friends, and rest. This project only exists because of genuine love for photography and the creative community.\n\n"
                "If ShaderLab has brought value to your workflow, saved you time, or helped you create shots you love, "
                "please consider supporting its development on Patreon. Every single contribution directly helps keep ShaderLab "
                "actively maintained, free, open-source, and accessible to everyone."
            );
            ImGui::PopTextWrapPos();

            ImGui::Spacing();
            ImGui::Spacing();

            // support on patreon button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.976f, 0.408f, 0.329f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.000f, 0.490f, 0.420f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.860f, 0.330f, 0.250f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 5.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

            if (ImGui::Button("Support on Patreon")) {
                ShellExecuteW(nullptr, L"open", L"https://www.patreon.com/cw/RayST", nullptr, nullptr, SW_SHOW);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("https://www.patreon.com/cw/RayST");
            }

            ImGui::SameLine();
            ImGui::PopStyleColor(4);

            if (ImGui::Button("GitHub Repository")) {
                ShellExecuteW(nullptr, L"open", L"https://github.com/NotRayST/ShaderLab", nullptr, nullptr, SW_SHOW);
            }

            ImGui::PopStyleVar(2);

            float total_h = ImGui::GetCursorPosY() - start_y + 18.0f;
            if (total_h > 100.0f) {
                s_about_note_height = total_h;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("Special thanks to originalnicodr, Alea | BeTa, and the others who have helped make this project possible.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();

}
