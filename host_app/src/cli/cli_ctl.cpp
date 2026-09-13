#include "cli_ctl.h"
#include "../../common/ipc_protocol.h"
#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <filesystem>
#include <algorithm>

#include "../../common/str_utils.h"

namespace fs = std::filesystem;
using str_utils::wide_to_utf8;

static const char *export_state_name(ExportState state) {
    switch (state) {
        case ExportState::Idle:      return "Idle";
        case ExportState::Requested: return "Requested";
        case ExportState::Loading:   return "Loading";
        case ExportState::Rendering: return "Rendering";
        case ExportState::Capturing: return "Capturing";
        case ExportState::Done:      return "Done";
        case ExportState::Failed:    return "Failed";
        default:                     return "Unknown";
    }
}

struct IpcConnection {
    HANDLE hMap = nullptr;
    SharedControlBlock *block = nullptr;
    DWORD pid = 0;

    ~IpcConnection() {
        if (block) UnmapViewOfFile(block);
        if (hMap) CloseHandle(hMap);
    }
};

static bool connect_to_host(IpcConnection &conn) {
    // check for main window class
    HWND hwnd = FindWindowW(L"ReShadeImagePipelineHostClass", nullptr);
    if (hwnd) {
        GetWindowThreadProcessId(hwnd, &conn.pid);
    }

    //if not found via window, iterate processes for ShaderLab.exe
    if (conn.pid == 0) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = { sizeof(pe) };
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"ShaderLab.exe") == 0 && pe.th32ProcessID != GetCurrentProcessId()) {
                        conn.pid = pe.th32ProcessID;
                        break;
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }

    wchar_t shm_name[kMaxPathW] = {};
    if (conn.pid != 0) {
        make_shared_mem_name_for_pid(shm_name, _countof(shm_name), conn.pid);
        conn.hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shm_name);
    }

    if (!conn.hMap) {
        make_shared_mem_name_active(shm_name, _countof(shm_name));
        conn.hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shm_name);
    }

    if (!conn.hMap) {
        return false;
    }

    conn.block = static_cast<SharedControlBlock *>(
        MapViewOfFile(conn.hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedControlBlock))
    );

    if (!conn.block) {
        CloseHandle(conn.hMap);
        conn.hMap = nullptr;
        return false;
    }

    if (conn.block->magic != kIpcMagic) {
        UnmapViewOfFile(conn.block);
        CloseHandle(conn.hMap);
        conn.block = nullptr;
        conn.hMap = nullptr;
        return false;
    }

    return true;
}

static bool dispatch_mailbox_command(
    SharedControlBlock *block,
    IpcCmd cmd,
    const char *target = nullptr,
    const float *values = nullptr,
    const wchar_t *path = nullptr,
    int timeout_ms = 4000)
{
    if (!block) return false;

    uint32_t req = block->mailbox.seq_request + 1;
    block->mailbox.cmd_id = static_cast<uint32_t>(cmd);
    block->mailbox.status = 0;
    block->mailbox.response_text[0] = L'\0';

    if (target) {
        strncpy_s(block->mailbox.target, target, _TRUNCATE);
    } else {
        block->mailbox.target[0] = '\0';
    }

    if (values) {
        for (int i = 0; i < 4; ++i) block->mailbox.values[i] = values[i];
    } else {
        for (int i = 0; i < 4; ++i) block->mailbox.values[i] = 0.0f;
    }

    if (path) {
        wcsncpy_s(block->mailbox.path, path, _TRUNCATE);
    } else {
        block->mailbox.path[0] = L'\0';
    }

    MemoryBarrier();
    block->mailbox.seq_request = req;

    auto start = std::chrono::steady_clock::now();
    while (block->mailbox.seq_handled != req) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeout_ms) {
            return false;
        }
    }

    // make sure we read status after matching seq_handled
    MemoryBarrier();
    return (block->mailbox.status > 0);
}

int CliCtl::execute(const CliOptions &opts) {
    IpcConnection conn;
    if (!connect_to_host(conn)) {
        if (opts.json_output) {
            std::cout << "{\n"
                      << "  \"success\": false,\n"
                      << "  \"error\": \"Could not connect to running ShaderLab host instance. Ensure ShaderLab is running.\"\n"
                      << "}\n";
        } else {
            std::cerr << "[Error] Could not find running ShaderLab instance. Ensure ShaderLab is running.\n";
        }
        return 1;
    }

    SharedControlBlock *b = conn.block;

    // determine command from extra_args or subcommand
    std::string action = "status";
    if (!opts.extra_args.empty()) {
        action = wide_to_utf8(opts.extra_args[0].c_str());
        std::transform(action.begin(), action.end(), action.begin(), [](unsigned char c) {
            return static_cast<char>(::tolower(c));
        });
    }

    if (action == "status") {
        std::string preview_u8 = wide_to_utf8(b->preview_path);
        std::string export_in_u8 = wide_to_utf8(b->export_input_path);
        std::string export_out_u8 = wide_to_utf8(b->export_output_path);
        std::string export_stat_u8 = wide_to_utf8(b->export_status);
        std::string host_stat_u8 = wide_to_utf8(b->host_status);

        bool lock_zoom = (b->view_interaction_flags & VIEW_FLAG_LOCK_ZOOM) != 0;
        bool lock_rot  = (b->view_interaction_flags & VIEW_FLAG_LOCK_ROT) != 0;
        bool lock_pan  = (b->view_interaction_flags & VIEW_FLAG_LOCK_PAN) != 0;
        bool depth_peek = (b->view_interaction_flags & VIEW_FLAG_DEPTH_PEEK) != 0;
        bool fine_mode  = (b->view_interaction_flags & VIEW_FLAG_FINE) != 0;

        if (opts.json_output) {
            std::cout << "{\n"
                      << "  \"success\": true,\n"
                      << "  \"pid\": " << conn.pid << ",\n"
                      << "  \"version\": " << b->version << ",\n"
                      << "  \"heartbeat\": " << b->heartbeat << ",\n"
                      << "  \"addon_heartbeat\": " << b->addon_heartbeat << ",\n"
                      << "  \"image\": {\n"
                      << "    \"path\": \"" << json_escape(preview_u8) << "\",\n"
                      << "    \"width\": " << b->view_image_width << ",\n"
                      << "    \"height\": " << b->view_image_height << "\n"
                      << "  },\n"
                      << "  \"viewport\": {\n"
                      << "    \"zoom\": " << b->view_zoom << ",\n"
                      << "    \"angle\": " << b->view_angle << ",\n"
                      << "    \"raw_angle\": " << b->view_raw_angle << ",\n"
                      << "    \"pan_x\": " << b->view_pan[0] << ",\n"
                      << "    \"pan_y\": " << b->view_pan[1] << ",\n"
                      << "    \"fullscreen\": " << (b->is_fullscreen ? "true" : "false") << ",\n"
                      << "    \"depth_peek\": " << (depth_peek ? "true" : "false") << ",\n"
                      << "    \"fine_mode\": " << (fine_mode ? "true" : "false") << ",\n"
                      << "    \"lock_zoom\": " << (lock_zoom ? "true" : "false") << ",\n"
                      << "    \"lock_rotate\": " << (lock_rot ? "true" : "false") << ",\n"
                      << "    \"lock_pan\": " << (lock_pan ? "true" : "false") << "\n"
                      << "  },\n"
                      << "  \"depth\": {\n"
                      << "    \"valid\": " << (b->depth_valid ? "true" : "false") << ",\n"
                      << "    \"far_plane\": " << b->depth_far_plane << ",\n"
                      << "    \"width\": " << b->depth_width << ",\n"
                      << "    \"height\": " << b->depth_height << "\n"
                      << "  },\n"
                      << "  \"reshade\": {\n"
                      << "    \"effects_enabled\": " << (b->effects_enabled ? "true" : "false") << ",\n"
                      << "    \"effects_compiling\": " << (b->effects_compiling ? "true" : "false") << ",\n"
                      << "    \"effects_ready\": " << b->effects_ready << "\n"
                      << "  },\n"
                      << "  \"export\": {\n"
                      << "    \"state\": \"" << export_state_name(b->export_state) << "\",\n"
                      << "    \"frame_index\": " << b->export_frame_index << ",\n"
                      << "    \"settle_cap\": " << b->export_settle_frames << ",\n"
                      << "    \"last_delta\": " << b->export_last_delta << ",\n"
                      << "    \"status\": \"" << json_escape(export_stat_u8) << "\"\n"
                      << "  }\n"
                      << "}\n";
        } else {
            std::cout << "========================================\n"
                      << "  ShaderLab Instance Status (PID: " << conn.pid << ")\n"
                      << "========================================\n"
                      << "Image:      " << (preview_u8.empty() ? "(none)" : preview_u8)
                      << " [" << b->view_image_width << "x" << b->view_image_height << "]\n"
                      << "Zoom:       " << b->view_zoom << "x\n"
                      << "Angle:      " << b->view_angle << " deg\n"
                      << "Pan:        [" << b->view_pan[0] << ", " << b->view_pan[1] << "]\n"
                      << "Locks:      Zoom=" << (lock_zoom ? "ON" : "OFF")
                      << " Rot=" << (lock_rot ? "ON" : "OFF")
                      << " Pan=" << (lock_pan ? "ON" : "OFF") << "\n"
                      << "Fullscreen: " << (b->is_fullscreen ? "YES" : "NO") << "\n"
                      << "Depth Map:  " << (b->depth_valid ? "Available (3D)" : "None (2D)")
                      << " [Far: " << b->depth_far_plane << "]\n"
                      << "ReShade:    " << (b->effects_enabled ? "Effects ON" : "Effects OFF")
                      << (b->effects_compiling ? " [Compiling...]" : " [Ready]") << "\n"
                      << "Export:     " << export_state_name(b->export_state)
                      << " (Frame " << b->export_frame_index << "/" << b->export_settle_frames << ")\n";
        }
        return 0;
    }

    if (action == "snapshot") {
        std::wstring out_path;
        if (opts.extra_args.size() >= 2) {
            out_path = opts.extra_args[1];
        } else if (!opts.output_path.empty()) {
            out_path = opts.output_path;
        } else {
            out_path = L"_snapshot.png";
        }

        fs::path abs_p = fs::absolute(out_path);
        bool ok = dispatch_mailbox_command(b, IpcCmd::Snapshot, nullptr, nullptr, abs_p.wstring().c_str(), 5000);
        std::string resp_u8 = wide_to_utf8(b->mailbox.response_text);

        if (opts.json_output) {
            std::cout << "{\n"
                      << "  \"success\": " << (ok ? "true" : "false") << ",\n"
                      << "  \"path\": \"" << json_escape(wide_to_utf8(abs_p.wstring().c_str())) << "\",\n"
                      << "  \"message\": \"" << json_escape(resp_u8) << "\"\n"
                      << "}\n";
        } else {
            if (ok) {
                std::cout << "[SUCCESS] Snapshot saved: " << wide_to_utf8(abs_p.wstring().c_str()) << " (" << resp_u8 << ")\n";
            } else {
                std::cerr << "[FAILED] Snapshot capture failed: " << resp_u8 << "\n";
            }
        }
        return ok ? 0 : 1;
    }

    if (action == "view") {
        float vals[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // zoom, angle, pan_x, pan_y
        vals[0] = (opts.extra_args.size() >= 2) ? static_cast<float>(_wtof(opts.extra_args[1].c_str())) : b->view_zoom;
        vals[1] = (opts.extra_args.size() >= 3) ? static_cast<float>(_wtof(opts.extra_args[2].c_str())) : b->view_angle;
        vals[2] = (opts.extra_args.size() >= 4) ? static_cast<float>(_wtoi(opts.extra_args[3].c_str())) : static_cast<float>(b->view_pan[0]);
        vals[3] = (opts.extra_args.size() >= 5) ? static_cast<float>(_wtoi(opts.extra_args[4].c_str())) : static_cast<float>(b->view_pan[1]);

        bool ok = dispatch_mailbox_command(b, IpcCmd::SetView, nullptr, vals, nullptr, 2000);
        if (opts.json_output) {
            std::cout << "{\n"
                      << "  \"success\": " << (ok ? "true" : "false") << ",\n"
                      << "  \"zoom\": " << b->view_zoom << ",\n"
                      << "  \"angle\": " << b->view_angle << ",\n"
                      << "  \"pan_x\": " << b->view_pan[0] << ",\n"
                      << "  \"pan_y\": " << b->view_pan[1] << "\n"
                      << "}\n";
        } else {
            std::cout << (ok ? "[SUCCESS] " : "[FAILED] ") << "View updated: Zoom=" << b->view_zoom
                      << ", Angle=" << b->view_angle << ", Pan=[" << b->view_pan[0] << ", " << b->view_pan[1] << "]\n";
        }
        return ok ? 0 : 1;
    }

    if (action == "action") {
        if (opts.extra_args.size() < 2) {
            std::cerr << "Error: No action name specified. Available actions: reset-rotation, reset-zoom, nudge-left, nudge-right, lock-pan, lock-zoom, lock-rot, lock-view, fullscreen, fine, save-project, save-as\n";
            return 1;
        }
        std::string act_name = wide_to_utf8(opts.extra_args[1].c_str());
        std::transform(act_name.begin(), act_name.end(), act_name.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });

        IpcAction act = IpcAction::ResetRotation;
        if (act_name == "reset-rotation" || act_name == "reset_rotation" || act_name == "rot-reset") act = IpcAction::ResetRotation;
        else if (act_name == "reset-zoom" || act_name == "reset_zoom" || act_name == "reset-pan" || act_name == "reset_zoom_pan") act = IpcAction::ResetZoomPan;
        else if (act_name == "undo") act = IpcAction::Undo;
        else if (act_name == "redo") act = IpcAction::Redo;
        else if (act_name == "nudge-left" || act_name == "nudge_left" || act_name == "left") act = IpcAction::NudgeLeft;
        else if (act_name == "nudge-right" || act_name == "nudge_right" || act_name == "right") act = IpcAction::NudgeRight;
        else if (act_name == "lock-pan" || act_name == "lock_pan") act = IpcAction::LockPan;
        else if (act_name == "lock-zoom" || act_name == "lock_zoom") act = IpcAction::LockZoom;
        else if (act_name == "lock-rot" || act_name == "lock_rot" || act_name == "lock-rotate") act = IpcAction::LockRotate;
        else if (act_name == "lock-view" || act_name == "lock_view" || act_name == "lock-all") act = IpcAction::LockView;
        else if (act_name == "fullscreen" || act_name == "toggle-fullscreen") act = IpcAction::ToggleFullscreen;
        else if (act_name == "fine" || act_name == "fine-tune") act = IpcAction::FineTune;
        else if (act_name == "save-project" || act_name == "save_project" || act_name == "save") act = IpcAction::SaveProject;
        else if (act_name == "save-as" || act_name == "save_as" || act_name == "save-project-as") act = IpcAction::SaveProjectAs;

        float vals[4] = { static_cast<float>(act), 0.0f, 0.0f, 0.0f };
        bool ok = dispatch_mailbox_command(b, IpcCmd::TriggerAction, nullptr, vals, nullptr, 2000);

        if (opts.json_output) {
            std::cout << "{\n"
                      << "  \"success\": " << (ok ? "true" : "false") << ",\n"
                      << "  \"action\": \"" << json_escape(act_name) << "\"\n"
                      << "}\n";
        } else {
            std::cout << (ok ? "[SUCCESS] " : "[FAILED] ") << "Executed action: " << act_name << "\n";
        }
        return ok ? 0 : 1;
    }

    if (action == "load") {
        if (opts.extra_args.size() < 2 && opts.input_path.empty()) {
            std::cerr << "Error: No file path specified for load.\n";
            return 1;
        }
        std::wstring p = (opts.extra_args.size() >= 2) ? opts.extra_args[1] : opts.input_path;
        fs::path abs_p = fs::absolute(p);
        bool ok = dispatch_mailbox_command(b, IpcCmd::LoadImage, nullptr, nullptr, abs_p.wstring().c_str(), 3000);
        std::string resp = wide_to_utf8(b->mailbox.response_text);

        if (opts.json_output) {
            std::cout << "{\n"
                      << "  \"success\": " << (ok ? "true" : "false") << ",\n"
                      << "  \"path\": \"" << json_escape(wide_to_utf8(abs_p.wstring().c_str())) << "\",\n"
                      << "  \"message\": \"" << json_escape(resp) << "\"\n"
                      << "}\n";
        } else {
            std::cout << (ok ? "[SUCCESS] " : "[FAILED] ") << resp << "\n";
        }
        return ok ? 0 : 1;
    }

    if (action == "export") {
        std::wstring out_p = (opts.extra_args.size() >= 2) ? opts.extra_args[1] : opts.output_path;
        if (out_p.empty()) out_p = L"rendered_export.png";
        fs::path abs_out = fs::absolute(out_p);

        float vals[4] = { static_cast<float>(opts.settle_frames), 0.0f, 0.0f, 0.0f };
        uint32_t flags = EXPORT_FLAG_WYSIWYG;
        if (opts.embed_png) flags |= EXPORT_FLAG_EMBED_DEPTH;
        if (opts.sidecar)   flags |= EXPORT_FLAG_DEPTH_SIDECAR;
        vals[1] = static_cast<float>(flags);

        bool ok = dispatch_mailbox_command(b, IpcCmd::TriggerExport, nullptr, vals, abs_out.wstring().c_str(), 3000);
        std::string resp = wide_to_utf8(b->mailbox.response_text);

        if (opts.json_output) {
            std::cout << "{\n"
                      << "  \"success\": " << (ok ? "true" : "false") << ",\n"
                      << "  \"output\": \"" << json_escape(wide_to_utf8(abs_out.wstring().c_str())) << "\",\n"
                      << "  \"message\": \"" << json_escape(resp) << "\"\n"
                      << "}\n";
        } else {
            std::cout << (ok ? "[SUCCESS] " : "[FAILED] ") << resp << "\n";
        }
        return ok ? 0 : 1;
    }

    if (action == "depth-peek" || action == "peek") {
        float val = 1.0f;
        if (opts.extra_args.size() >= 2) {
            val = static_cast<float>(_wtoi(opts.extra_args[1].c_str()));
        }
        float vals[4] = { val, 0.0f, 0.0f, 0.0f };
        bool ok = dispatch_mailbox_command(b, IpcCmd::SetDepthPeek, nullptr, vals, nullptr, 2000);
        if (opts.json_output) {
            std::cout << "{\n  \"success\": " << (ok ? "true" : "false") << ",\n  \"depth_peek\": " << (val > 0.5f ? "true" : "false") << "\n}\n";
        } else {
            std::cout << "[SUCCESS] Depth peek set to: " << (val > 0.5f ? "ON" : "OFF") << "\n";
        }
        return ok ? 0 : 1;
    }

    if (action == "resize") {
        if (opts.extra_args.size() < 3) {
            std::cerr << "Error: Specify width and height (e.g. ShaderLab.exe ctl resize 1280 720)\n";
            return 1;
        }
        float vals[4] = {
            static_cast<float>(_wtoi(opts.extra_args[1].c_str())),
            static_cast<float>(_wtoi(opts.extra_args[2].c_str())),
            0.0f, 0.0f
        };
        // swapchain recreation + reshade effects reload can take a few seconds
        bool ok = dispatch_mailbox_command(b, IpcCmd::ResizeWindow, nullptr, vals, nullptr, 10000);
        std::string resp = wide_to_utf8(b->mailbox.response_text);
        if (opts.json_output) {
            std::cout << "{\n  \"success\": " << (ok ? "true" : "false") << ",\n  \"message\": \"" << json_escape(resp) << "\"\n}\n";
        } else {
            std::cout << (ok ? "[SUCCESS] " : "[FAILED] ") << resp << "\n";
        }
        return ok ? 0 : 1;
    }

    if (action == "reshade-toggle") {
        float val = (b->effects_enabled != 0) ? 0.0f : 1.0f;
        if (opts.extra_args.size() >= 2) {
            val = static_cast<float>(_wtoi(opts.extra_args[1].c_str()));
        }
        float vals[4] = { val, 0.0f, 0.0f, 0.0f };
        bool ok = dispatch_mailbox_command(b, IpcCmd::ReshadeToggleEffects, nullptr, vals, nullptr, 2000);
        if (opts.json_output) {
            std::cout << "{\n  \"success\": " << (ok ? "true" : "false") << ",\n  \"effects_enabled\": " << (val > 0.5f ? "true" : "false") << "\n}\n";
        } else {
            std::cout << "[SUCCESS] ReShade effects set to: " << (val > 0.5f ? "ON" : "OFF") << "\n";
        }
        return ok ? 0 : 1;
    }

    if (action == "reshade-reload") {
        bool ok = dispatch_mailbox_command(b, IpcCmd::ReshadeReloadEffects, nullptr, nullptr, nullptr, 3000);
        if (opts.json_output) {
            std::cout << "{\n  \"success\": " << (ok ? "true" : "false") << "\n}\n";
        } else {
            std::cout << (ok ? "[SUCCESS] ReShade shaders reloading triggered\n" : "[FAILED] ReShade reload failed\n");
        }
        return ok ? 0 : 1;
    }

    if (action == "reshade-overlay") {
        float val = 1.0f;
        if (opts.extra_args.size() >= 2) {
            val = static_cast<float>(_wtoi(opts.extra_args[1].c_str()));
        }
        float vals[4] = { val, 0.0f, 0.0f, 0.0f };
        bool ok = dispatch_mailbox_command(b, IpcCmd::ReshadeToggleOverlay, nullptr, vals, nullptr, 2000);
        if (opts.json_output) {
            std::cout << "{\n  \"success\": " << (ok ? "true" : "false") << ",\n  \"overlay_open\": " << (val > 0.5f ? "true" : "false") << "\n}\n";
        } else {
            std::cout << "[SUCCESS] ReShade overlay toggled: " << (val > 0.5f ? "OPEN" : "CLOSED") << "\n";
        }
        return ok ? 0 : 1;
    }

    if (action == "reshade-technique") {
        if (opts.extra_args.size() < 2) {
            std::cerr << "Error: Specify technique name (e.g. ShaderLab.exe ctl reshade-technique DisplayDepth 1)\n";
            return 1;
        }
        std::string tech_name = wide_to_utf8(opts.extra_args[1].c_str());
        float val = 1.0f;
        if (opts.extra_args.size() >= 3) {
            val = static_cast<float>(_wtoi(opts.extra_args[2].c_str()));
        }
        float vals[4] = { val, 0.0f, 0.0f, 0.0f };
        bool ok = dispatch_mailbox_command(b, IpcCmd::ReshadeSetTechnique, tech_name.c_str(), vals, nullptr, 2000);
        std::string resp = wide_to_utf8(b->mailbox.response_text);
        if (opts.json_output) {
            std::cout << "{\n  \"success\": " << (ok ? "true" : "false") << ",\n  \"technique\": \"" << json_escape(tech_name) << "\",\n  \"state\": " << (val > 0.5f ? "true" : "false") << ",\n  \"message\": \"" << json_escape(resp) << "\"\n}\n";
        } else {
            std::cout << (ok ? "[SUCCESS] " : "[FAILED] ") << "Technique " << tech_name << " set to " << (val > 0.5f ? "ON" : "OFF") << " (" << resp << ")\n";
        }
        return ok ? 0 : 1;
    }

    if (action == "reshade-uniform") {
        if (opts.extra_args.size() < 3) {
            std::cerr << "Error: Specify uniform name and value (e.g. ShaderLab.exe ctl reshade-uniform iUIFar 1000.0)\n";
            return 1;
        }
        std::string var_name = wide_to_utf8(opts.extra_args[1].c_str());
        float vals[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (size_t i = 2; i < opts.extra_args.size() && (i - 2) < 4; ++i) {
            vals[i - 2] = static_cast<float>(_wtof(opts.extra_args[i].c_str()));
        }
        bool ok = dispatch_mailbox_command(b, IpcCmd::ReshadeSetUniform, var_name.c_str(), vals, nullptr, 2000);
        std::string resp = wide_to_utf8(b->mailbox.response_text);
        if (opts.json_output) {
            std::cout << "{\n  \"success\": " << (ok ? "true" : "false") << ",\n  \"uniform\": \"" << json_escape(var_name) << "\",\n  \"message\": \"" << json_escape(resp) << "\"\n}\n";
        } else {
            std::cout << (ok ? "[SUCCESS] " : "[FAILED] ") << "Uniform " << var_name << " updated (" << resp << ")\n";
        }
        return ok ? 0 : 1;
    }

    if (action == "reshade-preset") {
        if (opts.extra_args.size() < 2 && opts.preset_path.empty()) {
            std::cerr << "Error: Specify preset path (e.g. ShaderLab.exe ctl reshade-preset Cinematic.ini)\n";
            return 1;
        }
        std::wstring pr_path = (opts.extra_args.size() >= 2) ? opts.extra_args[1] : opts.preset_path;
        fs::path abs_pr = fs::absolute(pr_path);
        bool ok = dispatch_mailbox_command(b, IpcCmd::ReshadeSetPreset, nullptr, nullptr, abs_pr.wstring().c_str(), 2000);
        if (opts.json_output) {
            std::cout << "{\n  \"success\": " << (ok ? "true" : "false") << ",\n  \"preset\": \"" << json_escape(wide_to_utf8(abs_pr.wstring().c_str())) << "\"\n}\n";
        } else {
            std::cout << (ok ? "[SUCCESS] " : "[FAILED] ") << "Preset switched to: " << wide_to_utf8(abs_pr.wstring().c_str()) << "\n";
        }
        return ok ? 0 : 1;
    }

    std::cerr << "Unknown ctl subcommand: " << action << "\n"
              << "Run 'ShaderLab.exe ctl --help' for available subcommands.\n";
    return 1;
}
