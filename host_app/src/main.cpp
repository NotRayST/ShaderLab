#include "gfx_device.h"
#include "image_loader.h"
#include "blit_renderer.h"
#include "backbuffer_dump.h"
#include "settle_detector.h"
#include "viewport_controller.h"
#include "erase_tool.h"
#include "depth_manager.h"
#include "GUI/gui_manager.h"
#include "GUI/tool_requirements.h"

using CustomUI = gui::GuiManager;
using ShaderLabEffect = gui::ShaderLabEffect;
using ShaderLabUniform = gui::ShaderLabUniform;
#include "effect_pipeline.h"
#include "../../common/ipc_protocol.h"
#include "../../common/depth_chunk.h"
#include "../../common/depth_file.h"
#include "../../common/project_file.h"
#include "../../third_party/stb/stb_image_write.h"
#include <shellapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

static std::string wide_to_utf8(const std::wstring &wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

static std::wstring utf8_to_wide(const std::string &str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring out(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, out.data(), size);
    return out;
}

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <chrono>

namespace fs = std::filesystem;

struct StageEvent {
    std::wstring dir_key;
    size_t step = 0;
    size_t total = 0;
    std::wstring img_path;
    std::wstring sidecar_path;
};

struct PendingStageReload {
    std::mutex mtx;
    std::vector<StageEvent> queue;
};
static PendingStageReload g_pending_stage;

static void on_erase_stage_changed(const std::wstring &dir_key, size_t step, size_t total, const std::wstring &img_path, const std::wstring &sidecar_path) {
    std::lock_guard<std::mutex> lock(g_pending_stage.mtx);
    g_pending_stage.queue.push_back({ dir_key, step, total, img_path, sidecar_path });
}

static void set_host_status(SharedControlBlock *block, const std::wstring &msg) {
    if (!block) return;
    wcsncpy_s(block->host_status, kMaxPathW, msg.c_str(), _TRUNCATE);
    std::wcout << L"[Host] " << msg << L"\n";
}

static bool path_is_within(const fs::path &child, const fs::path &parent) {
    auto child_it = child.begin();
    auto parent_it = parent.begin();
    for (; parent_it != parent.end(); ++parent_it, ++child_it) {
        if (child_it == child.end() || *child_it != *parent_it)
            return false;
    }
    return true;
}

static bool is_erase_stage_path(const std::wstring &path) {
    std::wstring p = path;
    for (auto &c : p) c = towlower(c);
    return p.find(L"erase_stages") != std::wstring::npos;
}

static fs::path get_workspace_root() {
    wchar_t temp_dir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp_dir) == 0) {
        return {};
    }
    return fs::path(temp_dir) / "ShaderLab" / "workspace";
}

static void clean_stale_workspace_on_startup() {
    fs::path ws_dir = get_workspace_root();
    if (ws_dir.empty()) return;
    std::error_code ec;
    fs::create_directories(ws_dir, ec);
    for (const auto &entry : fs::directory_iterator(ws_dir, ec)) {
        fs::remove_all(entry.path(), ec);
    }
}

static void remove_doc_workspace_dir(const std::wstring &doc_dir_key) {
    if (doc_dir_key.empty()) return;
    fs::path ws_dir = get_workspace_root();
    if (ws_dir.empty()) return;
    std::error_code ec;
    fs::remove_all(ws_dir / doc_dir_key, ec);
}

static std::wstring prepare_working_copy(const std::wstring &source_path, const std::wstring &doc_dir_key = L"") {
    if (source_path.empty() || !fs::exists(source_path) || is_erase_stage_path(source_path)) {
        return source_path;
    }

    fs::path ws_dir = get_workspace_root();
    if (ws_dir.empty()) {
        return source_path;
    }
    std::error_code ec;
    fs::create_directories(ws_dir, ec);

    fs::path src(source_path);
    fs::path doc_ws_dir = doc_dir_key.empty() ? ws_dir : (ws_dir / doc_dir_key);
    if (doc_dir_key.empty() && path_is_within(src, ws_dir)) {
        return source_path;
    }

    if (!doc_dir_key.empty()) {
        if (path_is_within(src, doc_ws_dir)) {
            return source_path;
        }
        fs::remove_all(doc_ws_dir, ec);
        fs::create_directories(doc_ws_dir, ec);
    }

    fs::path dst = doc_ws_dir / src.filename();

    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return source_path;
    }

    fs::path src_sidecar = src;
    src_sidecar.replace_extension(L".sldepth");
    if (fs::exists(src_sidecar)) {
        fs::path dst_sidecar = dst;
        dst_sidecar.replace_extension(L".sldepth");
        fs::copy_file(src_sidecar, dst_sidecar, fs::copy_options::overwrite_existing, ec);
    }

    return dst.wstring();
}


struct ViewportHookContext {
    ViewportController *controller = nullptr;
    GfxDevice *gfx = nullptr;
    LoadedImage *active_image = nullptr;
    SharedControlBlock *block = nullptr;
    CustomUI *custom_ui = nullptr;
    uint32_t *last_synced_xform_ver = nullptr;
};

static uint64_t g_last_fullscreen_toggle_ms = 0;
static void toggle_fullscreen_safe(GfxDevice &gfx, SharedControlBlock *block) {
    uint64_t now_ms = GetTickCount64();
    if (now_ms - g_last_fullscreen_toggle_ms < 300) return;
    g_last_fullscreen_toggle_ms = now_ms;

    gfx.toggle_fullscreen();
    if (block) {
        block->is_fullscreen = gfx.is_fullscreen() ? 1u : 0u;
    }
}

static bool on_viewport_input_hook(void *user_data, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = static_cast<ViewportHookContext *>(user_data);
    if (!ctx || !ctx->controller || !ctx->gfx) return false;

    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        bool was_down   = (lParam & (1 << 30)) != 0;
        bool ctrl_down  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift_down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt_down   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

        if (ctx->controller->matches_keybind(IpcAction::ToggleFullscreen, wParam, ctrl_down, shift_down, alt_down) ||
            (!ctrl_down && !shift_down && !alt_down && wParam == VK_F11)) {
            if (!was_down) {
                toggle_fullscreen_safe(*ctx->gfx, ctx->block);
            }
            return true;
        }

        // save project as shortcut (ctrl+shift+s)
        if (ctx->controller->matches_keybind(IpcAction::SaveProjectAs, wParam, ctrl_down, shift_down, alt_down) ||
            (ctrl_down && shift_down && !alt_down && wParam == 'S')) {
            if (ctx->block && !was_down) {
                ctx->block->request_save_project_as++;
            }
            return true;
        }

        // save project shortcut (ctrl+s)
        if (ctx->controller->matches_keybind(IpcAction::SaveProject, wParam, ctrl_down, shift_down, alt_down) ||
            (ctrl_down && !shift_down && !alt_down && wParam == 'S')) {
            if (ctx->block && !was_down) {
                ctx->block->request_save_project++;
            }
            return true;
        }

        // export image as shortcut (ctrl+shift+e)
        if (ctx->controller->matches_keybind(IpcAction::ExportImageAs, wParam, ctrl_down, shift_down, alt_down) ||
            (ctrl_down && shift_down && !alt_down && wParam == 'E')) {
            if (ctx->block && !was_down) {
                ctx->block->request_export_image_as++;
            }
            return true;
        }

        // export image shortcut (ctrl+e)
        if (ctx->controller->matches_keybind(IpcAction::ExportImage, wParam, ctrl_down, shift_down, alt_down) ||
            (ctrl_down && !shift_down && !alt_down && wParam == 'E')) {
            if (ctx->block && !was_down) {
                ctx->block->request_export_image++;
            }
            return true;
        }

        // close tab shortcut (ctrl+w)
        if (ctrl_down && !shift_down && !alt_down && (wParam == 'W' || wParam == 'w')) {
            if (!was_down && ctx->custom_ui) {
                ctx->custom_ui->request_close_active_tab();
            }
            return true;
        }

        // cycle document tabs (ctrl+tab / ctrl+shift+tab)
        if (ctrl_down && !alt_down && wParam == VK_TAB) {
            if (!was_down && ctx->custom_ui) {
                ctx->custom_ui->request_cycle_tab(shift_down ? -1 : 1);
            }
            return true;
        }
    }

    uint32_t rw = ctx->gfx->get_render_width();
    uint32_t rh = ctx->gfx->get_render_height();
    uint32_t iw = ctx->active_image ? ctx->active_image->width : rw;
    uint32_t ih = ctx->active_image ? ctx->active_image->height : rh;

    bool handled = ctx->controller->handle_input(
        ctx->gfx->get_hwnd(),
        msg, wParam, lParam,
        rw, rh, iw, ih
    );

    if (handled) {
        // stamp idle timers so HUD appears only on actual active gestures
        if (ctx->block) {
            if (msg == WM_MOUSEWHEEL && !ctx->controller->is_lock_zoom()) {
                ctx->block->view_last_zoom_ms = GetTickCount64();
            }
            if (ctx->controller->is_rotating() || ctx->controller->is_rotate_locked_attempt()) {
                ctx->block->view_last_rotate_ms = GetTickCount64();
            }
            // sync native WndProc-driven changes into the SharedControlBlock so the addon
            // can still read them even with the overlay closed
            ctx->controller->sync_to_block(ctx->block);
            if (ctx->last_synced_xform_ver) {
                *ctx->last_synced_xform_ver = ctx->block->view_transform_version;
            }
        }
    } else {
        if (ctx->block && ctx->controller) {
            if (msg == WM_MOUSEWHEEL && ctx->controller->is_lock_zoom()) {
                // user tried to zoom while locked: light up ZoomHud showing zoom % and [LOCKED]
                ctx->block->view_last_zoom_ms = GetTickCount64();
            } else if ((msg == WM_RBUTTONDOWN || (msg == WM_LBUTTONDOWN && (GetAsyncKeyState(VK_MENU) & 0x8000)) ||
                        (msg == WM_KEYDOWN && (wParam == VK_LEFT || wParam == VK_RIGHT))) && ctx->controller->is_lock_rotate()) {
                // user tried to rotate while locked: show compass dial with [LOCKED]
                ctx->block->view_last_rotate_ms = GetTickCount64();
            }
        }
    }

    return handled;
}

#include "cli/cli_dispatcher.h"
#include <io.h>
#include <fcntl.h>

static void setup_cli_console() {
    bool has_parent_console = (AttachConsole(ATTACH_PARENT_PROCESS) != FALSE);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut && hOut != INVALID_HANDLE_VALUE && GetFileType(hOut) != FILE_TYPE_UNKNOWN) {
        int fd = _open_osfhandle((intptr_t)hOut, _O_TEXT);
        if (fd >= 0) {
            _dup2(fd, _fileno(stdout));
            _close(fd);
            setvbuf(stdout, nullptr, _IONBF, 0);
        }
    } else if (has_parent_console) {
        FILE *fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
    }

    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr && hErr != INVALID_HANDLE_VALUE && GetFileType(hErr) != FILE_TYPE_UNKNOWN) {
        int fd = _open_osfhandle((intptr_t)hErr, _O_TEXT);
        if (fd >= 0) {
            _dup2(fd, _fileno(stderr));
            _close(fd);
            setvbuf(stderr, nullptr, _IONBF, 0);
        }
    } else if (has_parent_console) {
        FILE *fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stderr);
    }

    std::ios::sync_with_stdio(true);
    std::cout.clear();
    std::cerr.clear();
    std::wcout.clear();
    std::wcerr.clear();
}

static void register_host_path_in_registry() {
    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        HKEY hKey = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ShaderLab", 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, L"ExecutablePath", 0, REG_SZ,
                           reinterpret_cast<const BYTE *>(exe_path),
                           static_cast<DWORD>((wcslen(exe_path) + 1) * sizeof(wchar_t)));

            fs::path parent = fs::path(exe_path).parent_path();
            std::wstring parent_str = parent.wstring();
            RegSetValueExW(hKey, L"InstallPath", 0, REG_SZ,
                           reinterpret_cast<const BYTE *>(parent_str.c_str()),
                           static_cast<DWORD>((parent_str.length() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        // Register shaderlab:// protocol handler under HKCU\Software\Classes\shaderlab
        HKEY hProto = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\shaderlab", 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hProto, nullptr) == ERROR_SUCCESS) {
            const wchar_t protoDesc[] = L"URL:ShaderLab Protocol";
            RegSetValueExW(hProto, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(protoDesc), sizeof(protoDesc));
            RegSetValueExW(hProto, L"URL Protocol", 0, REG_SZ, reinterpret_cast<const BYTE *>(L""), sizeof(wchar_t));

            HKEY hCmd = nullptr;
            if (RegCreateKeyExW(hProto, L"shell\\open\\command", 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hCmd, nullptr) == ERROR_SUCCESS) {
                std::wstring cmd = L"\"" + std::wstring(exe_path) + L"\" \"%1\"";
                RegSetValueExW(hCmd, nullptr, 0, REG_SZ,
                               reinterpret_cast<const BYTE *>(cmd.c_str()),
                               static_cast<DWORD>((cmd.length() + 1) * sizeof(wchar_t)));
                RegCloseKey(hCmd);
            }
            RegCloseKey(hProto);
        }
    }
}

static int run_app(int argc, wchar_t **argv) {
    if (!argv || argc <= 0) return 1;

    // check if invoked with CLI commands (render, test-shader, depth, info, preset, help, etc.)
    if (CliDispatcher::should_run_cli(argc, argv)) {
        setup_cli_console();
        return CliDispatcher::dispatch(argc, argv);
    }

    register_host_path_in_registry();

    // set up shared memory mapping for IPC / ctl automation
    wchar_t shm_name[kMaxPathW] = {};
    make_shared_mem_name(shm_name, _countof(shm_name));
    std::wcout << L"[Host] Creating shared memory mapping: " << shm_name << L"\n";

    HANDLE hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(SharedControlBlock),
        shm_name
    );

    if (!hMap) {
        std::wcerr << L"[Host] CreateFileMappingW failed! Error: " << GetLastError() << L"\n";
        return 1;
    }

    auto *control_block = static_cast<SharedControlBlock *>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedControlBlock))
    );

    if (!control_block) {
        std::wcerr << L"[Host] MapViewOfFile failed! Error: " << GetLastError() << L"\n";
        CloseHandle(hMap);
        return 1;
    }

    ZeroMemory(control_block, sizeof(SharedControlBlock));
    control_block->magic = kIpcMagic;
    control_block->version = kIpcVersion;
    control_block->host_flags = HOST_FLAG_ALIVE;
    control_block->export_state = ExportState::Idle;
    control_block->export_settle_frames = 900;
    control_block->export_flags = EXPORT_FLAG_WYSIWYG | EXPORT_FLAG_UNSYNCED;
    control_block->view_zoom = 1.0f;
    control_block->fine_tune_vk = VK_SHIFT;
    set_host_status(control_block, L"Host initialized. Ready.");

    // ensure common/ dir exists next to ShaderLab.exe
    wchar_t exe_file_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_file_path, MAX_PATH) > 0) {
        fs::path common_dir = fs::path(exe_file_path).parent_path() / "common";
        std::error_code ec;
        if (!fs::exists(common_dir)) {
            fs::create_directories(common_dir, ec);
        }
    }

    // d3d11 + swapchain, sized to the work area, not the fixed 960x540 window
    RECT work_area = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    uint32_t screen_w = static_cast<uint32_t>(work_area.right - work_area.left);
    uint32_t screen_h = static_cast<uint32_t>(work_area.bottom - work_area.top);

    uint32_t init_w = (std::min)(1280u, screen_w > 0 ? screen_w : 1280u);
    uint32_t init_h = (std::min)(720u, screen_h > 0 ? screen_h : 720u);

    GfxDevice gfx;
    if (!gfx.initialize(L"ShaderLab", init_w, init_h, init_w, init_h)) {
        std::wcerr << L"[Host] Failed to initialize graphics device\n";
        UnmapViewOfFile(control_block);
        CloseHandle(hMap);
        return 1;
    }

    BlitRenderer blit;
    if (!blit.initialize(gfx.get_device())) {
        std::wcerr << L"[Host] Failed to initialize blit renderer\n";
        UnmapViewOfFile(control_block);
        CloseHandle(hMap);
        return 1;
    }

    ComPtr<ID3D11RasterizerState> scissor_rs;
    D3D11_RASTERIZER_DESC rs_desc = {};
    rs_desc.FillMode = D3D11_FILL_SOLID;
    rs_desc.CullMode = D3D11_CULL_NONE;
    rs_desc.ScissorEnable = TRUE;
    rs_desc.DepthClipEnable = TRUE;
    gfx.get_device()->CreateRasterizerState(&rs_desc, &scissor_rs);

    LoadedImage active_image;
    LoadedImage original_image;
    std::wstring display_image_name;
    std::wstring current_image_path;

    struct AppDocument {
        uint32_t id = 0;
        std::wstring file_path;
        std::wstring source_path;
        std::wstring display_name;
        LoadedImage image;
        LoadedImage original_image;
        float zoom = 1.0f;
        float pan_x = 0.0f;
        float pan_y = 0.0f;
        float angle = 0.0f;
        bool viewport_initialized = false;
        std::vector<std::vector<ImVec2>> erase_strokes;
        std::vector<std::vector<std::vector<ImVec2>>> erase_undo_stack;
        std::vector<std::vector<std::vector<ImVec2>>> erase_redo_stack;
        EraseTool::DocumentState erase_state;
        std::vector<ShaderLabEffect> effects;
        int selected_effect_idx = 0;
        bool effects_initialized = false;
        bool depth_invert = false;
        bool live_depth_peek = false;
        bool master_bypass = false;
        std::string inpaint_status_text;
        std::string depth_status_text;
        bool is_dirty = false;
        uint32_t erase_step = 0;
        uint32_t erase_count = 0;
    };

    std::vector<AppDocument> app_documents;
    int active_doc_idx = -1;
    uint32_t next_doc_id = 1;

    ViewportController viewport;
    SettleDetector settle_detector;
    CustomUI custom_ui;
    EffectPipeline pipeline;

    uint32_t last_synced_xform_ver = 0;

    ViewportHookContext vp_ctx;
    vp_ctx.controller = &viewport;
    vp_ctx.gfx = &gfx;
    vp_ctx.active_image = &active_image;
    vp_ctx.block = control_block;
    vp_ctx.custom_ui = &custom_ui;
    vp_ctx.last_synced_xform_ver = &last_synced_xform_ver;

    custom_ui.initialize(gfx.get_hwnd(), gfx.get_device(), gfx.get_context());
    const std::vector<ShaderLabEffect> default_effects = custom_ui.get_effects();
    clean_stale_workspace_on_startup();
    CustomUI::ViewportCallbacks ui_vp_cb;
    ui_vp_cb.is_panning = [](void *p) { return static_cast<ViewportHookContext *>(p)->controller->is_panning(); };
    ui_vp_cb.is_capturing = [](void *p) { return static_cast<ViewportHookContext *>(p)->controller->is_capturing(); };
    ui_vp_cb.handle_input = on_viewport_input_hook;
    ui_vp_cb.cancel_input = [](void *p) {
        auto *c = static_cast<ViewportHookContext *>(p);
        c->controller->cancel_interaction();
        if (c->block) {
            c->controller->sync_to_block(c->block);
            if (c->last_synced_xform_ver) *c->last_synced_xform_ver = c->block->view_transform_version;
        }
    };
    ui_vp_cb.set_space_pan = [](void *p, bool active) {
        static_cast<ViewportHookContext *>(p)->controller->set_space_pan_active(active);
    };
    ui_vp_cb.user_data = &vp_ctx;
    custom_ui.set_viewport_callbacks(ui_vp_cb);

    pipeline.initialize(gfx.get_device());
    gfx.add_input_hook(CustomUI::input_hook, &custom_ui);
    EraseTool::get().set_stage_callback(on_erase_stage_changed);

    std::wcout << L"[Host] Graphics device initialized. Entering live render loop.\n";

    uint32_t last_preview_counter = 0;
    uint32_t last_request_toggle_fullscreen = 0;
    uint32_t settle_cap = 60;
    uint32_t export_canvas_w = 0;
    uint32_t export_canvas_h = 0;
    bool export_wysiwyg = true;
    ViewportTransform export_transform = {};
    uint32_t flush_frames_remaining = 0;  // extra frames rendered after convergence detected
    uint32_t export_start_effects_ready = 0;
    bool export_effects_initialized = false;
    uint32_t export_reload_wait_frames = 0;
    uint32_t export_capture_wait_frames = 0;
    uint32_t export_loading_frames = 0;
    auto last_export_tick = std::chrono::steady_clock::now();

    auto sync_depth_to_ipc = [&]() {
        uint64_t srv_handle = reinterpret_cast<uint64_t>(gfx.get_depth_canvas_srv());
        if (control_block->depth_srv_ptr != srv_handle ||
            control_block->depth_width != gfx.get_render_width() ||
            control_block->depth_height != gfx.get_render_height() ||
            control_block->depth_valid != (active_image.has_depth ? 1u : 0u))
        {
            control_block->depth_srv_ptr = srv_handle;
            control_block->depth_width = gfx.get_render_width();
            control_block->depth_height = gfx.get_render_height();
            control_block->depth_far_plane = active_image.far_plane;
            control_block->depth_valid = active_image.has_depth ? 1u : 0u;
            control_block->depth_version++;
        }
    };

    std::wstring current_window_title = L"";
    auto update_window_title = [&](const std::wstring &new_title) {
        if (new_title != current_window_title) {
            current_window_title = new_title;
            HWND hwnd = gfx.get_hwnd();
            if (hwnd) {
                SetWindowTextW(hwnd, current_window_title.c_str());
            }
        }
    };

    auto sync_tabs_to_ui = [&]() {
        std::vector<CustomUI::DocumentTab> ui_tabs;
        for (size_t i = 0; i < app_documents.size(); ++i) {
            const auto &d = app_documents[i];
            CustomUI::DocumentTab t;
            t.id = d.id;
            t.file_path = d.file_path;
            t.display_name = d.display_name;
            t.width = d.image.width;
            t.height = d.image.height;
            t.has_depth = d.image.has_depth;
            t.is_dirty = d.is_dirty || !d.erase_strokes.empty();
            ui_tabs.push_back(t);
        }
        custom_ui.set_tabs(ui_tabs, active_doc_idx);
    };

    auto doc_stage_dir_key = [](uint32_t id) -> std::wstring {
        return L"doc_" + std::to_wstring(id);
    };

    auto find_doc_by_id = [&](uint32_t id) -> int {
        for (int i = 0; i < static_cast<int>(app_documents.size()); ++i) {
            if (app_documents[i].id == id) return i;
        }
        return -1;
    };

    auto find_doc_by_dir_key = [&](const std::wstring &dir_key) -> int {
        for (int i = 0; i < static_cast<int>(app_documents.size()); ++i) {
            std::wstring key = app_documents[i].erase_state.stage_dir_key.empty()
                ? doc_stage_dir_key(app_documents[i].id)
                : app_documents[i].erase_state.stage_dir_key;
            if (_wcsicmp(key.c_str(), dir_key.c_str()) == 0) return i;
        }
        return -1;
    };

    auto find_open_doc_by_path = [&](const std::wstring &path) -> int {
        if (path.empty()) return -1;
        for (int i = 0; i < static_cast<int>(app_documents.size()); ++i) {
            if ((!app_documents[i].file_path.empty() && _wcsicmp(app_documents[i].file_path.c_str(), path.c_str()) == 0) ||
                (!app_documents[i].source_path.empty() && _wcsicmp(app_documents[i].source_path.c_str(), path.c_str()) == 0)) {
                return i;
            }
        }
        return -1;
    };

    auto save_active_doc_state = [&](int idx) {
        if (idx < 0 || idx >= static_cast<int>(app_documents.size())) return;
        auto &doc = app_documents[idx];
        doc.image = active_image;
        doc.original_image = original_image;
        doc.file_path = current_image_path;
        doc.display_name = display_image_name;
        doc.zoom = viewport.get_transform().zoom;
        doc.pan_x = viewport.get_transform().pan_x;
        doc.pan_y = viewport.get_transform().pan_y;
        doc.angle = viewport.get_transform().angle;
        doc.erase_strokes = custom_ui.get_erase_strokes();
        doc.erase_undo_stack = custom_ui.get_erase_undo_stack();
        doc.erase_redo_stack = custom_ui.get_erase_redo_stack();
        if (EraseTool::get().get_stage_dir_key().empty()) {
            EraseTool::get().set_stage_dir_key(doc_stage_dir_key(doc.id));
        }
        EraseTool::get().save_document_state(doc.erase_state);
        doc.effects = custom_ui.get_effects();
        doc.selected_effect_idx = custom_ui.get_selected_effect_idx();
        doc.depth_invert = custom_ui.is_depth_invert();
        doc.live_depth_peek = custom_ui.is_live_depth_peek();
        doc.master_bypass = custom_ui.is_master_bypass_active();
        if (!custom_ui.is_inpaint_busy()) {
            doc.inpaint_status_text = custom_ui.get_inpaint_status_text();
        }
        if (!custom_ui.is_depth_busy()) {
            doc.depth_status_text = custom_ui.get_depth_status_text();
        }
        doc.erase_step = static_cast<uint32_t>(doc.erase_state.stage_idx);
        doc.erase_count = doc.erase_state.stages.empty() ? 0u : static_cast<uint32_t>(doc.erase_state.stages.size() - 1);
        doc.is_dirty = doc.is_dirty || (doc.erase_step > 0) || !doc.erase_strokes.empty() || EraseTool::get().has_mask_edits();
    };

    auto restore_active_doc_state = [&](int idx, bool reset_viewport = false) {
        if (idx < 0 || idx >= static_cast<int>(app_documents.size())) {
            active_image.srv = nullptr;
            active_image.texture = nullptr;
            active_image.width = 0;
            active_image.height = 0;
            active_image.has_depth = false;
            active_image.depth_srv = nullptr;
            original_image = active_image;
            current_image_path.clear();
            display_image_name.clear();
            custom_ui.clear_pending_doc_requests();
            custom_ui.set_erase_strokes({}, {}, {});
            custom_ui.set_effects(default_effects);
            custom_ui.set_selected_effect_idx(0);
            custom_ui.set_depth_invert(false);
            custom_ui.set_live_depth_peek(false);
            custom_ui.set_master_bypass(false);
            custom_ui.set_inpaint_status_text("", false);
            custom_ui.set_depth_status_text("", false);
            custom_ui.set_depth_status(false, 0, 0, 1000.0f);
            EraseTool::DocumentState empty_erase_state;
            EraseTool::get().restore_document_state(empty_erase_state, gfx.get_device());
            if (control_block) {
                control_block->erase_history_step = 0;
                control_block->erase_history_count = 0;
                control_block->view_image_width = 0;
                control_block->view_image_height = 0;
                control_block->preview_path[0] = L'\0';
            }
            sync_depth_to_ipc();
            update_window_title(L"ShaderLab");
            return;
        }
        auto &doc = app_documents[idx];
        if (doc.erase_state.stage_dir_key.empty()) {
            doc.erase_state.stage_dir_key = doc_stage_dir_key(doc.id);
        }
        active_image = doc.image;
        original_image = doc.original_image;
        current_image_path = doc.file_path;
        display_image_name = doc.display_name;

        custom_ui.clear_pending_doc_requests();
        custom_ui.set_erase_strokes(doc.erase_strokes, doc.erase_undo_stack, doc.erase_redo_stack);
        EraseTool::get().restore_document_state(doc.erase_state, gfx.get_device());
        custom_ui.set_effects(doc.effects.empty() ? default_effects : doc.effects);
        custom_ui.set_selected_effect_idx(doc.selected_effect_idx);
        custom_ui.set_depth_invert(doc.depth_invert);
        custom_ui.set_live_depth_peek(doc.live_depth_peek);
        custom_ui.set_master_bypass(doc.master_bypass);

        bool this_doc_erasing = EraseTool::get().is_processing() &&
            (_wcsicmp(EraseTool::get().get_processing_dir_key().c_str(), doc.erase_state.stage_dir_key.c_str()) == 0);
        custom_ui.set_inpaint_status_text(
            this_doc_erasing ? EraseTool::get().get_status() : doc.inpaint_status_text,
            this_doc_erasing
        );
        custom_ui.set_depth_status_text(doc.depth_status_text, false);
        custom_ui.set_depth_status(active_image.has_depth, active_image.depth_width, active_image.depth_height, active_image.far_plane);
        if (control_block) {
            control_block->erase_history_step = static_cast<uint32_t>(EraseTool::get().get_current_erase_step());
            control_block->erase_history_count = static_cast<uint32_t>(EraseTool::get().get_total_erase_steps());
            doc.erase_step = control_block->erase_history_step;
            doc.erase_count = control_block->erase_history_count;
            wcsncpy_s(control_block->preview_path, kMaxPathW, doc.file_path.c_str(), _TRUNCATE);
        }

        if (reset_viewport || !doc.viewport_initialized) {
            viewport.zoom_fit(active_image.width, active_image.height);
            doc.viewport_initialized = true;
            doc.zoom = viewport.get_transform().zoom;
            doc.pan_x = viewport.get_transform().pan_x;
            doc.pan_y = viewport.get_transform().pan_y;
        } else {
            viewport.set_pan(doc.pan_x, doc.pan_y);
            viewport.set_zoom(doc.zoom);
        }

        control_block->view_image_width = active_image.width;
        control_block->view_image_height = active_image.height;
        control_block->depth_far_plane = active_image.far_plane;
        control_block->depth_valid = active_image.has_depth ? 1u : 0u;
        sync_depth_to_ipc();

        std::wstring depth_info = active_image.has_depth ? (L" [Depth Active, F=" + std::to_wstring(static_cast<int>(active_image.far_plane)) + L"]") : L"";
        update_window_title(L"ShaderLab - [" + display_image_name + L"] (" + std::to_wstring(active_image.width) + L"x" + std::to_wstring(active_image.height) + L")" + depth_info);
    };

    auto add_or_open_document = [&](const std::wstring &path, LoadedImage &&loaded, bool is_project = false, uint32_t preallocated_id = 0, const std::wstring &source_path = L"") {
        (void)is_project;
        if (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size())) {
            save_active_doc_state(active_doc_idx);
        }

        int existing_idx = find_open_doc_by_path(source_path.empty() ? path : source_path);
        if (existing_idx < 0) {
            existing_idx = find_open_doc_by_path(path);
        }
        if (existing_idx >= 0) {
            active_doc_idx = existing_idx;
            restore_active_doc_state(active_doc_idx, false);
            custom_ui.select_tab(active_doc_idx);
            sync_tabs_to_ui();
            return;
        }

        AppDocument doc;
        doc.id = (preallocated_id != 0) ? preallocated_id : next_doc_id++;
        doc.file_path = path;
        doc.source_path = source_path;
        std::wstring name_src = source_path.empty() ? path : source_path;
        doc.display_name = fs::path(name_src).filename().wstring();
        if (doc.display_name.empty()) {
            doc.display_name = L"Untitled-" + std::to_wstring(doc.id);
        }
        doc.image = std::move(loaded);
        doc.original_image = doc.image;
        doc.effects = default_effects;
        doc.selected_effect_idx = 0;
        doc.effects_initialized = true;
        doc.depth_invert = false;
        doc.live_depth_peek = false;
        doc.master_bypass = false;
        doc.viewport_initialized = false;
        doc.is_dirty = false;
        doc.erase_step = 0;
        doc.erase_count = 0;
        doc.erase_state.stage_dir_key = doc_stage_dir_key(doc.id);

        app_documents.push_back(std::move(doc));
        active_doc_idx = static_cast<int>(app_documents.size()) - 1;
        restore_active_doc_state(active_doc_idx, true);
        custom_ui.select_tab(active_doc_idx);
        sync_tabs_to_ui();
    };

    auto open_image_path_as_tab = [&](const std::wstring &orig_path, bool update_ipc = true) -> bool {
        if (orig_path.empty() || !fs::exists(orig_path)) return false;

        int existing_idx = find_open_doc_by_path(orig_path);
        if (existing_idx >= 0) {
            if (active_doc_idx != existing_idx) {
                if (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size())) {
                    save_active_doc_state(active_doc_idx);
                }
                active_doc_idx = existing_idx;
                restore_active_doc_state(active_doc_idx, false);
                custom_ui.select_tab(active_doc_idx);
                sync_tabs_to_ui();
            }
            if (update_ipc && control_block) {
                control_block->view_image_width = active_image.width;
                control_block->view_image_height = active_image.height;
                sync_depth_to_ipc();
            }
            return true;
        }

        uint32_t alloc_id = next_doc_id++;
        std::wstring doc_key = doc_stage_dir_key(alloc_id);
        std::wstring work_path = prepare_working_copy(orig_path, doc_key);
        LoadedImage new_preview;
        if (!ImageLoader::load_from_file(gfx.get_device(), work_path.c_str(), new_preview)) {
            remove_doc_workspace_dir(doc_key);
            return false;
        }

        add_or_open_document(work_path, std::move(new_preview), false, alloc_id, orig_path);
        if (update_ipc && control_block) {
            if (orig_path != work_path) {
                wcsncpy_s(control_block->dropped_file_path, kMaxPathW, orig_path.c_str(), _TRUNCATE);
                control_block->dropped_file_counter++;
            }
            wcsncpy_s(control_block->preview_path, kMaxPathW, work_path.c_str(), _TRUNCATE);
            control_block->active_project_path[0] = L'\0';
            control_block->project_dirty = 0;
            control_block->active_project_version++;
            control_block->view_image_width = active_image.width;
            control_block->view_image_height = active_image.height;
            sync_depth_to_ipc();
        }
        custom_ui.set_depth_status(active_image.has_depth, active_image.depth_width, active_image.depth_height, active_image.far_plane);
        std::wstring depth_info = active_image.has_depth ? (L" [Depth Active, F=" + std::to_wstring(static_cast<int>(active_image.far_plane)) + L"]") : L" [2D]";
        set_host_status(control_block, L"Loaded: " + display_image_name +
                        L" (" + std::to_wstring(active_image.width) + L"x" + std::to_wstring(active_image.height) + L")" + depth_info);
        update_window_title(L"ShaderLab - " + display_image_name);
        return true;
    };

    auto close_document = [&](int idx, bool force = false) {
        if (idx < 0 || idx >= static_cast<int>(app_documents.size())) return;
        save_active_doc_state(active_doc_idx);
        auto &doc = app_documents[idx];
        if (!force && doc.is_dirty) {
            std::wstring name = doc.display_name.empty() ? L"Untitled" : doc.display_name;
            std::wstring prompt = L"Document \"" + name + L"\" has unsaved changes.\n\nDo you want to save before closing?";
            int res = MessageBoxW(gfx.get_hwnd(), prompt.c_str(), L"ShaderLab - Unsaved Changes", MB_YESNOCANCEL | MB_ICONWARNING);
            if (res == IDCANCEL) {
                return;
            }
            if (res == IDYES) {
                std::wstring save_p;
                if (gui::save_file_dialog(gfx.get_hwnd(), L"PNG Image (*.png)\0*.png\0JPEG Image (*.jpg)\0*.jpg\0All Files (*.*)\0*.*\0", L"png", save_p)) {
                    BackbufferDump::capture_to_file(gfx, save_p.c_str(), gfx.get_render_width(), gfx.get_render_height());
                } else {
                    return; // user cancelled save dialog
                }
            }
        }

        std::wstring closed_doc_key = doc_stage_dir_key(doc.id);
        std::wstring closed_stage_key = doc.erase_state.stage_dir_key.empty() ? closed_doc_key : doc.erase_state.stage_dir_key;
        app_documents.erase(app_documents.begin() + idx);
        remove_doc_workspace_dir(closed_doc_key);
        if (!(EraseTool::get().is_processing() &&
              _wcsicmp(EraseTool::get().get_processing_dir_key().c_str(), closed_stage_key.c_str()) == 0)) {
            EraseTool::remove_stage_cache_dir(closed_stage_key);
        }

        if (app_documents.empty()) {
            active_doc_idx = -1;
            restore_active_doc_state(-1, false);
        } else {
            if (active_doc_idx >= static_cast<int>(app_documents.size())) {
                active_doc_idx = static_cast<int>(app_documents.size()) - 1;
            } else if (active_doc_idx > idx) {
                active_doc_idx--;
            }
            restore_active_doc_state(active_doc_idx, false);
            custom_ui.select_tab(active_doc_idx);
        }
        sync_tabs_to_ui();
    };

    auto close_other_documents = [&](int keep_idx) {
        if (keep_idx < 0 || keep_idx >= static_cast<int>(app_documents.size())) return;
        save_active_doc_state(active_doc_idx);
        for (int i = static_cast<int>(app_documents.size()) - 1; i >= 0; --i) {
            if (i == keep_idx) continue;
            close_document(i, false);
            if (i < keep_idx) keep_idx--;
        }
    };

    auto close_all_documents = [&]() {
        save_active_doc_state(active_doc_idx);
        for (int i = static_cast<int>(app_documents.size()) - 1; i >= 0; --i) {
            close_document(i, false);
        }
    };

    auto duplicate_document = [&](int idx) {
        if (idx < 0 || idx >= static_cast<int>(app_documents.size())) return;
        save_active_doc_state(active_doc_idx);
        AppDocument copy = app_documents[idx];
        copy.id = next_doc_id++;
        copy.display_name = copy.display_name + L" (Copy)";
        copy.source_path.clear();
        std::wstring old_key = app_documents[idx].erase_state.stage_dir_key.empty()
            ? doc_stage_dir_key(app_documents[idx].id)
            : app_documents[idx].erase_state.stage_dir_key;
        std::wstring new_key = doc_stage_dir_key(copy.id);
        if (!copy.file_path.empty() && fs::exists(copy.file_path) && !is_erase_stage_path(copy.file_path)) {
            copy.file_path = prepare_working_copy(copy.file_path, new_key);
        }
        EraseTool::copy_stage_cache_dir(old_key, new_key, copy.erase_state);
        if (!copy.file_path.empty() && !is_erase_stage_path(copy.erase_state.base_img)) {
            copy.erase_state.base_img = copy.file_path;
        }
        app_documents.push_back(std::move(copy));
        active_doc_idx = static_cast<int>(app_documents.size()) - 1;
        restore_active_doc_state(active_doc_idx, false);
        custom_ui.select_tab(active_doc_idx);
        sync_tabs_to_ui();
    };

    // check if launched with image or project file arguments
    std::vector<std::wstring> initial_launch_files;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i] || argv[i][0] == L'-') continue;
        std::wstring raw_arg = argv[i];
        if (raw_arg.rfind(L"shaderlab://", 0) == 0) {
            raw_arg = raw_arg.substr(12);
            if (raw_arg.rfind(L"open?path=", 0) == 0) {
                raw_arg = raw_arg.substr(10);
            }
            if (raw_arg.size() >= 2 && raw_arg.front() == L'"' && raw_arg.back() == L'"') {
                raw_arg = raw_arg.substr(1, raw_arg.size() - 2);
            }
        }
        if (fs::exists(raw_arg)) {
            initial_launch_files.push_back(raw_arg);
        }
    }

    std::wstring launch_path;
    if (!initial_launch_files.empty()) {
        launch_path = initial_launch_files[0];
    }

    if (!launch_path.empty()) {
        current_image_path = launch_path;
        active_image.srv = nullptr;
        wcsncpy_s(control_block->dropped_file_path, kMaxPathW, launch_path.c_str(), _TRUNCATE);
        control_block->dropped_file_counter = 1;
        wcsncpy_s(control_block->preview_path, kMaxPathW, launch_path.c_str(), _TRUNCATE);
        control_block->preview_counter = 1;
        last_preview_counter = 0;
    } else {
        // boot to the welcome screen (ascii art) until the user drops an image in
        active_image.srv = nullptr;
        wcsncpy_s(control_block->preview_path, kMaxPathW, L"", _TRUNCATE);
        control_block->preview_counter = 0;
        last_preview_counter = 0;
    }
    sync_depth_to_ipc();
    custom_ui.set_depth_status(active_image.has_depth, active_image.depth_width, active_image.depth_height, active_image.far_plane);

    bool running = true;
    const float idle_color[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
    auto last_time = std::chrono::steady_clock::now();

    while (running) {
        if (control_block) {
            if (control_block->export_state == ExportState::Idle && control_block->view_transform_version != last_synced_xform_ver) {
                last_synced_xform_ver = control_block->view_transform_version;
                viewport.sync_from_block(control_block);
            }
            static uint32_t s_last_synced_ba_ver = 0;
            if (control_block->before_after_version != s_last_synced_ba_ver) {
                s_last_synced_ba_ver = control_block->before_after_version;
                viewport.sync_from_block(control_block);
            }
        }

        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            if (msg.message == WM_CLOSE && msg.hwnd == gfx.get_hwnd()) {
                save_active_doc_state(active_doc_idx);
                bool any_dirty = false;
                for (const auto &d : app_documents) {
                    if (d.is_dirty) { any_dirty = true; break; }
                }
                if (any_dirty) {
                    int res = MessageBoxW(gfx.get_hwnd(), L"You have documents with unsaved changes.\n\nDo you want to discard changes and exit?", L"ShaderLab - Exit Confirmation", MB_YESNO | MB_ICONWARNING);
                    if (res != IDYES) {
                        continue; // ignore close
                    }
                }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!running) break;

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        dt = (std::min)(dt, 0.1f); // cap dt so a hiccup doesnt send the fade animation flying

        control_block->heartbeat++;

        // keep the fine-tune key synced
        viewport.set_fine_vk(control_block->fine_tune_vk);

        // poll f11 directly as fallback
        static bool s_f11_prev = false;
        bool f11_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11_down && !s_f11_prev) {
            DWORD fg_pid = 0;
            HWND fg = GetForegroundWindow();
            if (fg) GetWindowThreadProcessId(fg, &fg_pid);
            if (fg_pid == GetCurrentProcessId()) {
                toggle_fullscreen_safe(gfx, control_block);
            }
        }
        s_f11_prev = f11_down;

        // sync the view transform if ipc modified it
        if (control_block->export_state == ExportState::Idle && control_block->view_transform_version != last_synced_xform_ver) {
            last_synced_xform_ver = control_block->view_transform_version;
            viewport.sync_from_block(control_block);
        }

        // track active project baseline for unsaved changes in title bar
        static uint32_t s_saved_proj_depth_ver = 0;
        static uint32_t s_last_active_project_ver = 0;

        if (control_block->active_project_version != s_last_active_project_ver) {
            s_last_active_project_ver = control_block->active_project_version;
            s_saved_proj_depth_ver = control_block->depth_version;
            control_block->project_dirty = 0;
        }

        if (control_block->active_project_path[0] != L'\0') {
            bool depth_changed = (control_block->depth_version != s_saved_proj_depth_ver);
            if (depth_changed) {
                control_block->project_dirty = 1;
            }
        }

        if (control_block->request_toggle_fullscreen != last_request_toggle_fullscreen) {
            last_request_toggle_fullscreen = control_block->request_toggle_fullscreen;
            toggle_fullscreen_safe(gfx, control_block);
        }

        // process host-targeted mailbox commands from CLI / MCP / automation
        static bool s_pending_ipc_snapshot = false;
        static wchar_t s_pending_snapshot_path[kMaxPathW] = {};

        if (control_block->mailbox.seq_request != control_block->mailbox.seq_handled) {
            uint32_t cmd = control_block->mailbox.cmd_id;
            if (cmd == static_cast<uint32_t>(IpcCmd::Snapshot)) {
                // defer capture to end of frame so complete UI is drawn and buffer is not cleared by flip discard
                s_pending_ipc_snapshot = true;
                wcsncpy_s(s_pending_snapshot_path, kMaxPathW, control_block->mailbox.path, _TRUNCATE);
            } else if (cmd == static_cast<uint32_t>(IpcCmd::DumpDom)) {
                std::string json = custom_ui.get_dom_json();
                std::filesystem::path p(control_block->mailbox.path);
                if (p.empty()) p = "ui_dom.json";
                std::ofstream ofs(p);
                if (ofs.is_open()) {
                    ofs << json;
                    ofs.close();
                    control_block->mailbox.status = 1;
                    swprintf_s(control_block->mailbox.response_text, L"DOM dumped to: %s", p.wstring().c_str());
                } else {
                    control_block->mailbox.status = -1;
                    swprintf_s(control_block->mailbox.response_text, L"Failed to write DOM JSON");
                }
                MemoryBarrier();
                control_block->mailbox.seq_handled = control_block->mailbox.seq_request;
            } else if (cmd == static_cast<uint32_t>(IpcCmd::ClickElement)) {
                std::string target_id = control_block->mailbox.target;
                bool ok = custom_ui.click_dom_element(target_id);
                control_block->mailbox.status = ok ? 1 : -1;
                if (ok) {
                    swprintf_s(control_block->mailbox.response_text, L"Clicked DOM element: %s", utf8_to_wide(target_id).c_str());
                } else {
                    swprintf_s(control_block->mailbox.response_text, L"DOM element not found: %s", utf8_to_wide(target_id).c_str());
                }
                MemoryBarrier();
                control_block->mailbox.seq_handled = control_block->mailbox.seq_request;
            } else if (cmd == static_cast<uint32_t>(IpcCmd::SimulateMouse)) {
                float mx = control_block->mailbox.values[0];
                float my = control_block->mailbox.values[1];
                int act = static_cast<int>(control_block->mailbox.values[2]);
                custom_ui.queue_mouse_event(mx, my, act);
                viewport.simulate_mouse(mx, my, act);
                custom_ui.simulate_canvas_mouse(mx, my, act);
                viewport.sync_to_block(control_block);
                last_synced_xform_ver = control_block->view_transform_version;
                control_block->mailbox.status = 1;
                swprintf_s(control_block->mailbox.response_text, L"Mouse event: (%.0f, %.0f) act=%d", mx, my, act);
                MemoryBarrier();
                control_block->mailbox.seq_handled = control_block->mailbox.seq_request;
            } else if (cmd == static_cast<uint32_t>(IpcCmd::SetView)) {
                float z = control_block->mailbox.values[0];
                float a = control_block->mailbox.values[1];
                int px = static_cast<int>(control_block->mailbox.values[2]);
                int py = static_cast<int>(control_block->mailbox.values[3]);
                if (z > 0.0f) {
                    control_block->view_zoom = z;
                }
                control_block->view_angle = a;
                control_block->view_raw_angle = a;
                control_block->view_pan[0] = px;
                control_block->view_pan[1] = py;
                control_block->view_transform_version++;
                viewport.sync_from_block(control_block);
                last_synced_xform_ver = control_block->view_transform_version;
                control_block->mailbox.status = 1;
                swprintf_s(control_block->mailbox.response_text, L"View updated");
                control_block->mailbox.seq_handled = control_block->mailbox.seq_request;
            } else if (cmd == static_cast<uint32_t>(IpcCmd::TriggerAction)) {
                uint32_t action_id = static_cast<uint32_t>(control_block->mailbox.values[0]);
                if (action_id == static_cast<uint32_t>(IpcAction::Undo)) {
                    custom_ui.undo_erase_stroke();
                } else if (action_id == static_cast<uint32_t>(IpcAction::Redo)) {
                    custom_ui.redo_erase_stroke();
                } else if (action_id == static_cast<uint32_t>(IpcAction::ResetRotation)) {
                    control_block->view_angle = 0.0f;
                    control_block->view_raw_angle = 0.0f;
                    control_block->view_transform_version++;
                    viewport.sync_from_block(control_block);
                    last_synced_xform_ver = control_block->view_transform_version;
                } else if (action_id == static_cast<uint32_t>(IpcAction::ResetZoomPan)) {
                    control_block->view_zoom = 1.0f;
                    control_block->view_pan[0] = 0;
                    control_block->view_pan[1] = 0;
                    control_block->view_transform_version++;
                    viewport.sync_from_block(control_block);
                    last_synced_xform_ver = control_block->view_transform_version;

                } else if (action_id == static_cast<uint32_t>(IpcAction::LockPan)) {
                    ipc_toggle_view_flag(&control_block->view_interaction_flags, VIEW_FLAG_LOCK_PAN);
                } else if (action_id == static_cast<uint32_t>(IpcAction::LockZoom)) {
                    ipc_toggle_view_flag(&control_block->view_interaction_flags, VIEW_FLAG_LOCK_ZOOM);
                } else if (action_id == static_cast<uint32_t>(IpcAction::LockRotate)) {
                    ipc_toggle_view_flag(&control_block->view_interaction_flags, VIEW_FLAG_LOCK_ROT);
                } else if (action_id == static_cast<uint32_t>(IpcAction::LockView)) {
                    uint32_t all_mask = (VIEW_FLAG_LOCK_PAN | VIEW_FLAG_LOCK_ZOOM | VIEW_FLAG_LOCK_ROT);
                    if ((control_block->view_interaction_flags & all_mask) == all_mask) {
                        ipc_clear_view_flag(&control_block->view_interaction_flags, all_mask);
                    } else {
                        ipc_set_view_flag(&control_block->view_interaction_flags, all_mask);
                    }
                } else if (action_id == static_cast<uint32_t>(IpcAction::ToggleFullscreen)) {
                    toggle_fullscreen_safe(gfx, control_block);
                } else if (action_id == static_cast<uint32_t>(IpcAction::FineTune)) {
                    ipc_toggle_view_flag(&control_block->view_interaction_flags, VIEW_FLAG_FINE);
                } else if (action_id == static_cast<uint32_t>(IpcAction::NudgeLeft)) {
                    if (!(control_block->view_interaction_flags & VIEW_FLAG_LOCK_PAN)) {
                        float step = (control_block->view_interaction_flags & VIEW_FLAG_FINE) ? 2.0f : 15.0f;
                        control_block->view_pan[0] -= static_cast<int32_t>(step);
                        control_block->view_transform_version++;
                        viewport.sync_from_block(control_block);
                        last_synced_xform_ver = control_block->view_transform_version;
                    }
                } else if (action_id == static_cast<uint32_t>(IpcAction::NudgeRight)) {
                    if (!(control_block->view_interaction_flags & VIEW_FLAG_LOCK_PAN)) {
                        float step = (control_block->view_interaction_flags & VIEW_FLAG_FINE) ? 2.0f : 15.0f;
                        control_block->view_pan[0] += static_cast<int32_t>(step);
                        control_block->view_transform_version++;
                        viewport.sync_from_block(control_block);
                        last_synced_xform_ver = control_block->view_transform_version;
                    }

                } else if (action_id == static_cast<uint32_t>(IpcAction::SaveProject)) {
                    control_block->request_save_project++;
                } else if (action_id == static_cast<uint32_t>(IpcAction::SaveProjectAs)) {
                    control_block->request_save_project_as++;
                } else if (action_id == static_cast<uint32_t>(IpcAction::ToggleBeforeAfter)) {
                    viewport.toggle_before_after();
                    viewport.sync_to_block(control_block);
                    last_synced_xform_ver = control_block->view_transform_version;
                }
                control_block->mailbox.status = 1;
                control_block->mailbox.seq_handled = control_block->mailbox.seq_request;
            } else if (cmd == static_cast<uint32_t>(IpcCmd::SetDepthPeek)) {
                bool peek = (control_block->mailbox.values[0] > 0.5f);
                if (peek) ipc_set_view_flag(&control_block->view_interaction_flags, VIEW_FLAG_DEPTH_PEEK);
                else ipc_clear_view_flag(&control_block->view_interaction_flags, VIEW_FLAG_DEPTH_PEEK);
                control_block->mailbox.status = 1;
                control_block->mailbox.seq_handled = control_block->mailbox.seq_request;
            } else if (cmd == static_cast<uint32_t>(IpcCmd::ResizeWindow)) {
                uint32_t rw = static_cast<uint32_t>(control_block->mailbox.values[0]);
                uint32_t rh = static_cast<uint32_t>(control_block->mailbox.values[1]);
                if (rw >= 320 && rh >= 240) {
                    if (gfx.is_fullscreen()) {
                        gfx.set_fullscreen(false);
                        control_block->is_fullscreen = 0;
                    }
                    RECT rc = { 0, 0, static_cast<LONG>(rw), static_cast<LONG>(rh) };
                    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
                    SetWindowPos(gfx.get_hwnd(), nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
                    gfx.resize_buffers(rw, rh);
                    control_block->mailbox.status = 1;
                    swprintf_s(control_block->mailbox.response_text, L"Resized to %ux%u", rw, rh);
                } else {
                    control_block->mailbox.status = -1;
                    swprintf_s(control_block->mailbox.response_text, L"Invalid dimensions");
                }
                // memory barrier so status and response text are visible to other processes before seq_handled
                MemoryBarrier();
                control_block->mailbox.seq_handled = control_block->mailbox.seq_request;
            } else if (cmd == static_cast<uint32_t>(IpcCmd::LoadImage)) {
                if (wcslen(control_block->mailbox.path) > 0 && fs::exists(control_block->mailbox.path)) {
                    wcsncpy_s(control_block->dropped_file_path, kMaxPathW, control_block->mailbox.path, _TRUNCATE);
                    control_block->dropped_file_counter++;
                    wcsncpy_s(control_block->preview_path, kMaxPathW, control_block->mailbox.path, _TRUNCATE);
                    control_block->preview_counter++;
                    control_block->mailbox.status = 1;
                    swprintf_s(control_block->mailbox.response_text, L"Loaded: %s", control_block->mailbox.path);
                } else {
                    control_block->mailbox.status = -1;
                    swprintf_s(control_block->mailbox.response_text, L"File not found");
                }
                control_block->mailbox.seq_handled = control_block->mailbox.seq_request;
            } else if (cmd == static_cast<uint32_t>(IpcCmd::TriggerExport)) {
                wcsncpy_s(control_block->export_output_path, kMaxPathW, control_block->mailbox.path, _TRUNCATE);
                control_block->export_settle_frames = static_cast<uint32_t>(control_block->mailbox.values[0]);
                control_block->export_flags = static_cast<uint32_t>(control_block->mailbox.values[1]);
                wcsncpy_s(control_block->export_input_path, kMaxPathW, control_block->preview_path, _TRUNCATE);
                control_block->export_state = ExportState::Requested;
                control_block->mailbox.status = 1;
                swprintf_s(control_block->mailbox.response_text, L"Export started");
                control_block->mailbox.seq_handled = control_block->mailbox.seq_request;
            }
        }

        // handle drag & drop onto host window
        std::wstring dropped_file;
        int drop_x = 0, drop_y = 0;
        if (gfx.get_and_clear_dropped_file(dropped_file, drop_x, drop_y)) {
            if (fs::exists(dropped_file)) {
                wcsncpy_s(control_block->dropped_file_path, kMaxPathW, dropped_file.c_str(), _TRUNCATE);
                control_block->dropped_file_x = drop_x;
                control_block->dropped_file_y = drop_y;
                control_block->dropped_file_counter++;

                wcsncpy_s(control_block->preview_path, kMaxPathW, dropped_file.c_str(), _TRUNCATE);
                control_block->preview_counter++;
            }
        }

        // new preview image or project requested
        if (control_block->preview_counter != last_preview_counter) {
            last_preview_counter = control_block->preview_counter;
            if (wcslen(control_block->preview_path) > 0 && fs::exists(control_block->preview_path)) {
                if (project_file::is_project_file(control_block->preview_path)) {
                    std::wstring proj_src_path = control_block->preview_path;
                    int existing_proj_idx = find_open_doc_by_path(proj_src_path);
                    if (existing_proj_idx >= 0) {
                        if (active_doc_idx != existing_proj_idx) {
                            save_active_doc_state(active_doc_idx);
                            active_doc_idx = existing_proj_idx;
                            restore_active_doc_state(active_doc_idx, false);
                            custom_ui.select_tab(active_doc_idx);
                            sync_tabs_to_ui();
                        }
                    } else {
                        uint32_t alloc_id = next_doc_id++;
                        fs::path ws_dir = get_workspace_root();
                        fs::path proj_ws = ws_dir.empty() ? fs::path() : (ws_dir / doc_stage_dir_key(alloc_id));
                        std::error_code ec;
                        if (!proj_ws.empty()) {
                            fs::remove_all(proj_ws, ec);
                            fs::create_directories(proj_ws, ec);
                        }

                        std::wstring out_img_path, out_preset_path, load_err;
                        project_file::ProjectManifest manifest;
                        if (project_file::load_project(proj_src_path, proj_ws.wstring(), out_img_path, out_preset_path, manifest, load_err)) {
                            LoadedImage new_preview;
                            if (ImageLoader::load_from_file(gfx.get_device(), out_img_path.c_str(), new_preview)) {
                                add_or_open_document(out_img_path, std::move(new_preview), true, alloc_id, proj_src_path);
                                if (manifest.depth.has_depth && manifest.depth.far_plane > 0.0f) {
                                    active_image.far_plane = manifest.depth.far_plane;
                                    if (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size())) {
                                        app_documents[active_doc_idx].image.far_plane = manifest.depth.far_plane;
                                        app_documents[active_doc_idx].image.has_depth = true;
                                    }
                                }
                                control_block->view_image_width = active_image.width;
                                control_block->view_image_height = active_image.height;
                                sync_depth_to_ipc();
                                custom_ui.set_depth_status(active_image.has_depth, active_image.depth_width, active_image.depth_height, active_image.far_plane);
                                sync_tabs_to_ui();

                                // restore camera transform and interaction locks
                                control_block->view_zoom = manifest.view.zoom > 0.0f ? manifest.view.zoom : 1.0f;
                                control_block->view_angle = manifest.view.angle;
                                control_block->view_raw_angle = manifest.view.angle;
                                control_block->view_pan[0] = static_cast<int32_t>(std::round(manifest.view.pan[0]));
                                control_block->view_pan[1] = static_cast<int32_t>(std::round(manifest.view.pan[1]));

                                uint32_t flags = 0;
                                if (manifest.view.lock_zoom) flags |= VIEW_FLAG_LOCK_ZOOM;
                                if (manifest.view.lock_rot)  flags |= VIEW_FLAG_LOCK_ROT;
                                if (manifest.view.lock_pan)  flags |= VIEW_FLAG_LOCK_PAN;
                                constexpr uint32_t kProjectLockMask = VIEW_FLAG_LOCK_ZOOM | VIEW_FLAG_LOCK_ROT | VIEW_FLAG_LOCK_PAN;
                                ipc_update_view_flags(&control_block->view_interaction_flags, kProjectLockMask, flags);
                                control_block->view_transform_version++;
                                viewport.sync_from_block(control_block);

                                // update preview_path to point to the extracted working image
                                wcsncpy_s(control_block->preview_path, kMaxPathW, out_img_path.c_str(), _TRUNCATE);

                                // track active project path so Ctrl+S saves directly to this project
                                fs::path abs_proj = fs::absolute(proj_src_path);
                                wcsncpy_s(control_block->active_project_path, kMaxPathW, abs_proj.wstring().c_str(), _TRUNCATE);
                                control_block->project_dirty = 0;
                                control_block->active_project_version++;

                                // restore reshade preset if present
                                if (control_block->export_state == ExportState::Idle && !out_preset_path.empty() && fs::exists(out_preset_path)) {
                                    fs::path abs_preset = fs::absolute(out_preset_path);
                                    wcsncpy_s(control_block->requested_preset_path, kMaxPathW, abs_preset.wstring().c_str(), _TRUNCATE);
                                    control_block->requested_preset_version++;
                                }

                                std::wstring depth_info = active_image.has_depth ? (L" [Depth Active, F=" + std::to_wstring(static_cast<int>(active_image.far_plane)) + L"]") : L" [2D]";
                                set_host_status(control_block, L"Loaded Project: " + display_image_name + depth_info);
                                update_window_title(L"ShaderLab - " + display_image_name);
                            } else {
                                remove_doc_workspace_dir(doc_stage_dir_key(alloc_id));
                                set_host_status(control_block, L"Project error: failed to decode image inside " + fs::path(proj_src_path).filename().wstring());
                            }
                        } else {
                            remove_doc_workspace_dir(doc_stage_dir_key(alloc_id));
                            set_host_status(control_block, L"Failed to load project: " + load_err);
                        }
                    }
                } else if (is_erase_stage_path(control_block->preview_path)) {
                    LoadedImage new_preview;
                    if (ImageLoader::load_from_file(gfx.get_device(), control_block->preview_path, new_preview)) {
                        active_image = new_preview;
                        if (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size())) {
                            app_documents[active_doc_idx].image = active_image;
                            app_documents[active_doc_idx].is_dirty = true;
                        }
                        sync_tabs_to_ui();
                        control_block->view_image_width = active_image.width;
                        control_block->view_image_height = active_image.height;
                        sync_depth_to_ipc();
                        custom_ui.set_depth_status(active_image.has_depth, active_image.depth_width, active_image.depth_height, active_image.far_plane);
                    }
                } else {
                    std::wstring orig_source_path = control_block->preview_path;
                    if (open_image_path_as_tab(orig_source_path, true)) {
                        static bool s_loaded_initial_extra_files = false;
                        if (!s_loaded_initial_extra_files && initial_launch_files.size() > 1) {
                            s_loaded_initial_extra_files = true;
                            for (size_t k = 1; k < initial_launch_files.size(); ++k) {
                                open_image_path_as_tab(initial_launch_files[k], false);
                            }
                            if (!app_documents.empty()) {
                                save_active_doc_state(active_doc_idx);
                                active_doc_idx = 0;
                                restore_active_doc_state(0, false);
                                custom_ui.select_tab(0);
                                sync_tabs_to_ui();
                            }
                        }
                    }
                }
            }
        }

        sync_depth_to_ipc();
    custom_ui.set_depth_status(active_image.has_depth, active_image.depth_width, active_image.depth_height, active_image.far_plane);

        if (control_block && control_block->export_state != ExportState::Idle) {
            ipc_clear_view_flag(&control_block->view_interaction_flags, VIEW_FLAG_DEPTH_PEEK);
        }

        // export state machine
        switch (control_block->export_state) {
        case ExportState::Requested: {
            export_capture_wait_frames = 0;
            export_loading_frames = 0;
            last_export_tick = std::chrono::steady_clock::now();

            control_block->export_state = ExportState::Loading;
            update_window_title(L"ShaderLab - Loading Image for Export...");
            set_host_status(control_block, L"Export: loading image " + std::wstring(control_block->export_input_path));

            if (!ImageLoader::load_from_file(gfx.get_device(), control_block->export_input_path, active_image)) {
                control_block->export_error = IPC_ERR_LOAD_FAILED;
                control_block->export_state = ExportState::Failed;
                set_host_status(control_block, L"Export failed: could not load image");
                break;
            }

            viewport.set_locked(true);
            gfx.set_processing_image(true);

            export_wysiwyg = (control_block->export_flags & EXPORT_FLAG_WYSIWYG) != 0;
            export_transform = viewport.get_transform();
            if (export_wysiwyg) {
                ViewportController::get_rotated_aabb(
                    active_image.width,
                    active_image.height,
                    viewport.get_snapped_angle(),
                    export_canvas_w,
                    export_canvas_h
                );
            } else {
                export_canvas_w = active_image.width;
                export_canvas_h = active_image.height;
            }

            if (!gfx.resize_buffers(export_canvas_w, export_canvas_h)) {
                control_block->export_error = IPC_ERR_RESIZE_FAILED;
                control_block->export_state = ExportState::Failed;
                gfx.set_processing_image(false);
                viewport.set_locked(false);
                set_host_status(control_block, L"Export failed: swap chain resize failed");
                break;
            }

            sync_depth_to_ipc();
    custom_ui.set_depth_status(active_image.has_depth, active_image.depth_width, active_image.depth_height, active_image.far_plane);

            if ((control_block->export_flags & EXPORT_FLAG_AUTO_CONVERGE) != 0) {
                settle_cap = 1800u; // 30s safety timeout for Smart Settle (ignores user settle time)
            } else {
                settle_cap = (control_block->export_settle_frames > 0) ? control_block->export_settle_frames : 900;
                settle_cap = std::clamp(settle_cap, 10u, 3600u);
            }

            export_start_effects_ready = control_block->effects_ready;
            export_reload_wait_frames = 0;
            export_effects_initialized = false;
            control_block->export_frame_index = 0;
            control_block->export_converged = 0;
            control_block->export_last_delta = 1.0f;
            flush_frames_remaining = 0;
            settle_detector.reset();

            // stay in loading: next loop iterations present warmup frames so reshade and
            // the d3d pipeline settle before convergence and rendering begin
            break;
        }
        case ExportState::Rendering: {
            gfx.set_depth_render_target();
            gfx.clear_depth_canvas(1.0f);
            if (active_image.has_depth && active_image.depth_srv) {
                if (export_wysiwyg) {
                    blit.render_depth_transformed(
                        gfx.get_context(),
                        active_image.depth_srv.Get(),
                        export_transform,
                        static_cast<float>(export_canvas_w),
                        static_cast<float>(export_canvas_h),
                        static_cast<float>(active_image.width),
                        static_cast<float>(active_image.height),
                        active_image.far_plane
                    );
                } else {
                    blit.render_depth(gfx.get_context(), active_image.depth_srv.Get(), active_image.far_plane);
                }
            }

            gfx.set_render_target();
            gfx.clear(idle_color);

            if (export_wysiwyg) {
                blit.render_transformed(
                    gfx.get_context(),
                    active_image.srv.Get(),
                    export_transform,
                    static_cast<float>(export_canvas_w),
                    static_cast<float>(export_canvas_h),
                    static_cast<float>(active_image.width),
                    static_cast<float>(active_image.height)
                );
            } else {
                blit.render(gfx.get_context(), active_image.srv.Get());
            }

            // vsync=1 so temporal shaders (RTGI, DoF, bloom) get accurate ~16.6ms deltas
            gfx.present(1, 0);

            bool unsynced = (control_block->export_flags & EXPORT_FLAG_UNSYNCED) != 0;
            if (!unsynced) {
                auto now_export = std::chrono::steady_clock::now();
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_export - last_export_tick).count();
                if (elapsed_ms >= 0 && elapsed_ms < 16) {
                    Sleep(static_cast<DWORD>(16 - elapsed_ms));
                }
                last_export_tick = std::chrono::steady_clock::now();
            }

            if (!export_effects_initialized) {
                export_effects_initialized = true;
                control_block->export_frame_index = 0;
                settle_detector.reset();
            }

            control_block->export_frame_index++;

            float cur_sec = static_cast<float>(control_block->export_frame_index) / 60.0f;
            float total_sec = static_cast<float>(settle_cap) / 60.0f;
            int pct = (settle_cap > 0) ? static_cast<int>((control_block->export_frame_index * 100) / settle_cap) : 0;
            pct = std::clamp(pct, 0, 100);
            wchar_t title_buf[256];
            bool auto_converge = (control_block->export_flags & EXPORT_FLAG_AUTO_CONVERGE) != 0;
            if (auto_converge) {
                swprintf_s(title_buf, L"ShaderLab - Exporting (Settling %.1fs - Smart Settle)...", cur_sec);
            } else {
                swprintf_s(title_buf, L"ShaderLab - Exporting (Settling %.1fs / %.1fs - %d%%)...", cur_sec, total_sec, pct);
            }
            update_window_title(title_buf);

            bool converged = false;
            float delta = 1.0f;
            static constexpr uint32_t kFlushFrames = 30;

            if (flush_frames_remaining > 0) {
                flush_frames_remaining--;
                if (flush_frames_remaining == 0) {
                    control_block->export_state = ExportState::Capturing;
                }
            } else if (auto_converge) {
                // smart settle convergence analysis
                converged = settle_detector.check_convergence(
                    gfx.get_device(),
                    gfx.get_context(),
                    gfx.get_swap_chain(),
                    delta,
                    false
                );
                control_block->export_last_delta = delta;

                if (converged) {
                    control_block->export_converged = 1;
                    flush_frames_remaining = kFlushFrames;
                } else if (control_block->export_frame_index >= settle_cap) {
                    // safety timeout reached (30s)
                    control_block->export_converged = 0;
                    control_block->export_last_delta = settle_detector.get_last_delta();
                    control_block->export_state = ExportState::Capturing;
                }
            } else if (!auto_converge && control_block->export_frame_index >= settle_cap) {
                // manual settle mode: waits the user selected seconds gracefully
                control_block->export_converged = 0;
                flush_frames_remaining = kFlushFrames;
            }
            break;
        }
        case ExportState::Capturing: {
            wchar_t cap_title[256];
            swprintf_s(cap_title, L"ShaderLab - Capturing Native %ux%u...", export_canvas_w, export_canvas_h);
            update_window_title(cap_title);

            gfx.set_depth_render_target();
            gfx.clear_depth_canvas(1.0f);
            if (active_image.has_depth && active_image.depth_srv) {
                if (export_wysiwyg) {
                    blit.render_depth_transformed(
                        gfx.get_context(),
                        active_image.depth_srv.Get(),
                        viewport.get_transform(),
                        static_cast<float>(export_canvas_w),
                        static_cast<float>(export_canvas_h),
                        static_cast<float>(active_image.width),
                        static_cast<float>(active_image.height),
                        active_image.far_plane
                    );
                } else {
                    blit.render_depth(gfx.get_context(), active_image.depth_srv.Get(), active_image.far_plane);
                }
            }

            gfx.set_render_target();
            gfx.clear(idle_color);

            if (export_wysiwyg) {
                blit.render_transformed(
                    gfx.get_context(),
                    active_image.srv.Get(),
                    viewport.get_transform(),
                    static_cast<float>(export_canvas_w),
                    static_cast<float>(export_canvas_h),
                    static_cast<float>(active_image.width),
                    static_cast<float>(active_image.height)
                );
            } else {
                blit.render(gfx.get_context(), active_image.srv.Get());
            }

            export_capture_wait_frames++;

            if (control_block->export_state == ExportState::Capturing) {
                    bool ok = BackbufferDump::capture_to_file(gfx, control_block->export_output_path, export_canvas_w, export_canvas_h);
                    if (ok) {
                        bool req_embed = (control_block->export_flags & EXPORT_FLAG_EMBED_DEPTH) != 0;
                        bool req_sidecar = (control_block->export_flags & EXPORT_FLAG_DEPTH_SIDECAR) != 0;
                        if ((req_embed || req_sidecar) && active_image.has_depth && !active_image.depth_pixels.empty()) {
                            std::string out_utf8 = wide_to_utf8(control_block->export_output_path);
                            std::string ext = std::filesystem::path(control_block->export_output_path).extension().string();
                            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
                            bool is_png = (ext == ".png");
                            if (!is_png && req_embed) {
                                req_sidecar = true; // non-PNG fallback
                            }
                            if (is_png && req_embed) {
                                DepthMapHeader c_hdr = {};
                                c_hdr.magic = kDepthMagicSLD1;
                                c_hdr.version = kDepthVersion1;
                                c_hdr.encoding = 0;
                                c_hdr.width = active_image.depth_width;
                                c_hdr.height = active_image.depth_height;
                                c_hdr.flags = kDepthFlagValid;
                                c_hdr.near_plane = 1.0f;
                                c_hdr.far_plane = active_image.far_plane;
                                c_hdr.raw_byte_size = static_cast<uint32_t>(active_image.depth_pixels.size() * sizeof(float));
                                c_hdr.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count());
                                strcpy_s(c_hdr.game_name, "ShaderLab");
                                std::string inj_err;
                                depth_chunk::inject_sldp(out_utf8, c_hdr, active_image.depth_pixels.data(), active_image.depth_pixels.size(), inj_err);
                            }
                            if (req_sidecar) {
                                std::filesystem::path sidecar_p(control_block->export_output_path);
                                sidecar_p.replace_extension(".sldepth");
                                SidecarDepthHeader s_hdr = {};
                                s_hdr.magic = kSidecarMagic;
                                s_hdr.version = kSidecarVersion;
                                s_hdr.encoding = 0;
                                s_hdr.width = active_image.depth_width;
                                s_hdr.height = active_image.depth_height;
                                s_hdr.flags = kSidecarFlagValid;
                                s_hdr.far_plane_used = active_image.far_plane;
                                s_hdr.raw_byte_size = static_cast<uint32_t>(active_image.depth_pixels.size() * sizeof(float));
                                s_hdr.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count());
                                strcpy_s(s_hdr.game_name, "ShaderLab");
                                std::string side_err;
                                depth_file::write_sidecar(wide_to_utf8(sidecar_p.wstring().c_str()), s_hdr, active_image.depth_pixels.data(), active_image.depth_pixels.size(), side_err);
                            }
                        } else {
                            // clean up stale sidecar if depth was disabled
                            std::filesystem::path sidecar_p(control_block->export_output_path);
                            sidecar_p.replace_extension(".sldepth");
                            if (std::filesystem::exists(sidecar_p)) {
                                std::error_code ec;
                                std::filesystem::remove(sidecar_p, ec);
                            }
                        }
                        control_block->export_error = IPC_OK;
                        std::wstring status_msg = L"Export complete: " + std::filesystem::path(control_block->export_output_path).filename().wstring();
                        wcsncpy_s(control_block->export_status, kMaxPathW, status_msg.c_str(), _TRUNCATE);
                        control_block->export_state = ExportState::Done;
                    } else {
                        control_block->export_error = IPC_ERR_WRITE_FAILED;
                        wcsncpy_s(control_block->export_status, kMaxPathW, L"Export failed (file write error)", _TRUNCATE);
                        control_block->export_state = ExportState::Failed;
                    }
                    export_capture_wait_frames = 0;
                }

            gfx.present(1, 0);
            break;
        }
        case ExportState::Done:
        case ExportState::Failed: {
            export_capture_wait_frames = 0;
            export_loading_frames = 0;
            if (control_block->export_state == ExportState::Done) {
                update_window_title(L"ShaderLab - Export Complete!");
            } else {
                update_window_title(L"ShaderLab - Export Failed!");
            }
            viewport.set_locked(false);
            gfx.set_processing_image(false);
            settle_detector.release_resources();
            control_block->export_state = ExportState::Idle;
            sync_depth_to_ipc();
    custom_ui.set_depth_status(active_image.has_depth, active_image.depth_width, active_image.depth_height, active_image.far_plane);
            break;
        }
        case ExportState::Loading: {
            export_loading_frames++;
            if (export_loading_frames > 300) {
                control_block->export_error = IPC_ERR_TIMEOUT;
                control_block->export_state = ExportState::Failed;
                set_host_status(control_block, L"Export failed: loading timed out");
                break;
            }

            gfx.set_depth_render_target();
            gfx.clear_depth_canvas(1.0f);
            if (active_image.has_depth && active_image.depth_srv) {
                if (export_wysiwyg) {
                    blit.render_depth_transformed(
                        gfx.get_context(),
                        active_image.depth_srv.Get(),
                        viewport.get_transform(),
                        static_cast<float>(export_canvas_w),
                        static_cast<float>(export_canvas_h),
                        static_cast<float>(active_image.width),
                        static_cast<float>(active_image.height),
                        active_image.far_plane
                    );
                } else {
                    blit.render_depth(gfx.get_context(), active_image.depth_srv.Get(), active_image.far_plane);
                }
            }

            gfx.set_render_target();
            gfx.clear(idle_color);

            if (active_image.srv) {
                if (export_wysiwyg) {
                    blit.render_transformed(
                        gfx.get_context(),
                        active_image.srv.Get(),
                        viewport.get_transform(),
                        static_cast<float>(export_canvas_w),
                        static_cast<float>(export_canvas_h),
                        static_cast<float>(active_image.width),
                        static_cast<float>(active_image.height)
                    );
                } else {
                    blit.render(gfx.get_context(), active_image.srv.Get());
                }
            }

            UINT sync = (control_block->export_flags & EXPORT_FLAG_UNSYNCED) ? 0 : 1;
            gfx.present(sync, 0);

            // Present 2 warmup frames so ReShade and the swapchain settle onto the new canvas size
            if (export_loading_frames >= 2) {
                export_start_effects_ready = control_block->effects_ready;
                export_reload_wait_frames = 0;
                export_effects_initialized = false;
                control_block->export_frame_index = 0;
                control_block->export_converged = 0;
                control_block->export_last_delta = 1.0f;
                flush_frames_remaining = 0;
                settle_detector.reset();
                control_block->export_state = ExportState::Rendering;
            }
            break;
        }
        case ExportState::Idle:
        default: {
            export_capture_wait_frames = 0;
            export_loading_frames = 0;

            DWORD fg_pid = 0;
            HWND fg_hwnd = GetForegroundWindow();
            if (fg_hwnd) GetWindowThreadProcessId(fg_hwnd, &fg_pid);
            bool is_app_focused = (fg_pid == GetCurrentProcessId());
            bool want_text = (control_block->view_interaction_flags & VIEW_FLAG_TEXT_INPUT) != 0;
            bool is_d_pressed = (GetAsyncKeyState('D') & 0x8000) != 0;
            bool depth_peek = active_image.has_depth && active_image.depth_srv && is_d_pressed && is_app_focused && !want_text;
            if (control_block) {
                if (depth_peek) {
                    ipc_set_view_flag(&control_block->view_interaction_flags, VIEW_FLAG_DEPTH_PEEK);
                } else {
                    ipc_clear_view_flag(&control_block->view_interaction_flags, VIEW_FLAG_DEPTH_PEEK);
                }
            }

            if (active_image.srv != nullptr) {
                bool has_project = (control_block->active_project_path[0] != L'\0');
                std::wstring fname;
                if (has_project) {
                    fs::path p(control_block->active_project_path);
                    fname = p.filename().wstring();
                } else {
                    if (!display_image_name.empty()) {
                        fname = display_image_name;
                    } else {
                        fs::path p(control_block->preview_path);
                        fname = p.filename().wstring();
                    }
                }
                if (fname.empty()) fname = L"Image";

                bool is_dirty = has_project && (control_block->project_dirty != 0);
                std::wstring dirty_marker = is_dirty ? L"*" : L"";
                std::wstring title = L"ShaderLab - [" + fname + dirty_marker + L"] (" +
                                     std::to_wstring(active_image.width) + L"x" +
                                     std::to_wstring(active_image.height) + L")";
                if (active_image.has_depth) {
                    title += depth_peek ? L" [Depth Peek Active]" : L" [Depth Active]";
                }
                if (viewport.is_before_after_enabled()) {
                    float a = viewport.get_before_after_angle();
                    const wchar_t *sides = L" [Left: Before | Right: After]";
                    if (a >= 45.0f && a < 135.0f) sides = L" [Top: Before | Bottom: After]";
                    else if (a >= 135.0f && a < 225.0f) sides = L" [Right: Before | Left: After]";
                    else if (a >= 225.0f && a < 315.0f) sides = L" [Bottom: Before | Top: After]";
                    title += sides;
                }
                if (is_dirty) {
                    title += L" [unsaved changes]";
                }
                update_window_title(title);
            } else {
                update_window_title(L"ShaderLab");
            }

            // before canvas (for un-erased comparison in before/after split)
            bool ba_enabled = (control_block->view_interaction_flags & VIEW_FLAG_BEFORE_AFTER) != 0;
            if (ba_enabled && original_image.srv && control_block->erase_history_step > 0 && !depth_peek) {
                gfx.set_before_render_target();
                gfx.clear_before_canvas(idle_color);
                blit.render_transformed(
                    gfx.get_context(),
                    original_image.srv.Get(),
                    viewport.get_transform(),
                    static_cast<float>(gfx.get_render_width()),
                    static_cast<float>(gfx.get_render_height()),
                    static_cast<float>(original_image.width),
                    static_cast<float>(original_image.height)
                );
                control_block->before_canvas_srv_ptr = reinterpret_cast<uint64_t>(gfx.get_before_canvas_srv());
            } else {
                control_block->before_canvas_srv_ptr = 0;
            }

            gfx.set_depth_render_target();
            gfx.clear_depth_canvas(1.0f);
            if (active_image.has_depth && active_image.depth_srv) {
                blit.render_depth_transformed(
                    gfx.get_context(),
                    active_image.depth_srv.Get(),
                    viewport.get_transform(),
                    static_cast<float>(gfx.get_render_width()),
                    static_cast<float>(gfx.get_render_height()),
                    static_cast<float>(active_image.width),
                    static_cast<float>(active_image.height),
                    active_image.far_plane
                );
            }

            gfx.set_render_target();
            gfx.clear(idle_color);

            float menu_h = custom_ui.get_menu_bar_height();
            float tool_options_h = custom_ui.get_tool_options_height();
            float toolbar_w = custom_ui.get_toolbar_width();
            float status_bar_h = custom_ui.get_status_bar_height();
            float left_w = custom_ui.get_left_panel_width(gfx.get_render_width());
            float right_w = custom_ui.get_right_panel_width(gfx.get_render_width());

            float viewable_x = toolbar_w + left_w;
            float viewable_y = menu_h + tool_options_h;
            float viewable_w = static_cast<float>(gfx.get_render_width()) - viewable_x - right_w;
            float viewable_h = static_cast<float>(gfx.get_render_height()) - viewable_y - status_bar_h;
            if (viewable_w < 64.0f) viewable_w = 64.0f;
            if (viewable_h < 64.0f) viewable_h = 64.0f;

            viewport.set_viewable_bounds(viewable_x, viewable_y, viewable_w, viewable_h);
            viewport.set_static_zoom(custom_ui.is_static_zoom_active());
            viewport.set_before_after_enabled(custom_ui.is_before_after_active());
            viewport.set_erase_active(custom_ui.is_erase_active());
            viewport.set_hand_tool_active(custom_ui.get_active_tool() == CustomUI::ActiveTool::Hand);

            std::wstring open_path;
            if (custom_ui.pop_open_image_request(open_path)) {
                open_image_path_as_tab(open_path, true);
            }

            int switch_to_tab = -1;
            if (custom_ui.pop_switch_tab_request(switch_to_tab)) {
                if (switch_to_tab >= 0 && switch_to_tab < static_cast<int>(app_documents.size()) && switch_to_tab != active_doc_idx) {
                    save_active_doc_state(active_doc_idx);
                    active_doc_idx = switch_to_tab;
                    restore_active_doc_state(active_doc_idx, false);
                    sync_tabs_to_ui();
                }
            }

            int close_tab_idx = -1;
            if (custom_ui.pop_close_tab_request(close_tab_idx)) {
                close_document(close_tab_idx);
            }

            int keep_tab_idx = -1;
            if (custom_ui.pop_close_other_tabs_request(keep_tab_idx)) {
                close_other_documents(keep_tab_idx);
            }

            if (custom_ui.pop_close_all_tabs_request()) {
                close_all_documents();
            }

            int dup_tab_idx = -1;
            if (custom_ui.pop_duplicate_tab_request(dup_tab_idx)) {
                duplicate_document(dup_tab_idx);
            }

            std::wstring save_path;
            if (custom_ui.pop_save_snapshot_request(save_path)) {
                BackbufferDump::capture_to_file(gfx, save_path.c_str(), gfx.get_render_width(), gfx.get_render_height());
                if (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size())) {
                    app_documents[active_doc_idx].is_dirty = false;
                    sync_tabs_to_ui();
                }
            }

            std::wstring attach_path;
            if (custom_ui.pop_attach_depth_request(attach_path)) {
                LoadedImage depth_img;
                if (ImageLoader::load_from_file(gfx.get_device(), attach_path.c_str(), depth_img)) {
                    active_image.has_depth = true;
                    active_image.depth_srv = depth_img.srv;
                    active_image.depth_texture = depth_img.texture;
                    active_image.depth_width = depth_img.width;
                    active_image.depth_height = depth_img.height;
                    if (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size())) {
                        app_documents[active_doc_idx].image = active_image;
                    }
                    sync_depth_to_ipc();
                    custom_ui.set_depth_status(true, depth_img.width, depth_img.height, active_image.far_plane);
                    sync_tabs_to_ui();
                }
            }

            if (custom_ui.pop_reset_view_request()) {
                viewport.reset();
            }
            if (custom_ui.pop_fit_view_request()) {
                viewport.zoom_fit(active_image.width, active_image.height);
            }
            if (custom_ui.pop_100_view_request()) {
                viewport.zoom_100(active_image.width, active_image.height);
            }
            if (custom_ui.pop_200_view_request()) {
                viewport.zoom_200(active_image.width, active_image.height);
            }
            if (custom_ui.pop_zoom_in_request()) {
                viewport.zoom_by(1.25f);
            }
            if (custom_ui.pop_zoom_out_request()) {
                viewport.zoom_by(0.80f);
            }

            if (custom_ui.pop_clear_mask_request()) {
                custom_ui.clear_erase_strokes();
                if (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size())) {
                    app_documents[active_doc_idx].erase_strokes.clear();
                    app_documents[active_doc_idx].erase_undo_stack.clear();
                    app_documents[active_doc_idx].erase_redo_stack.clear();
                    EraseTool::get().save_document_state(app_documents[active_doc_idx].erase_state);
                    sync_tabs_to_ui();
                }
            }

            std::wstring add_shader_p;
            if (custom_ui.pop_add_shader_request(add_shader_p)) {
                // Future shader load hook
            }

            static bool s_was_depth_processing = false;
            static uint32_t s_depth_target_doc_id = 0;
            static size_t s_depth_target_erase_step = 0;
            static std::wstring s_depth_target_path;
            static bool s_was_erase_processing = false;
            static uint32_t s_erase_target_doc_id = 0;

            std::string depth_enc;
            float depth_far = 1000.0f;
            if (custom_ui.pop_generate_depth_request(depth_enc, depth_far)) {
                std::wstring active_path = (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size()))
                    ? app_documents[active_doc_idx].file_path : current_image_path;
                size_t cur_erase_step = EraseTool::get().get_current_erase_step();
                if (cur_erase_step > 0) {
                    std::wstring stage_p = EraseTool::get().get_current_stage_path();
                    if (!stage_p.empty() && fs::exists(stage_p)) {
                        active_path = stage_p;
                    }
                }
                if (active_path.empty() || !fs::exists(active_path)) {
                    custom_ui.set_depth_status_text("Save or open an image file before generating depth.", false);
                    if (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size())) {
                        app_documents[active_doc_idx].depth_status_text = custom_ui.get_depth_status_text();
                    }
                } else {
                    s_depth_target_doc_id = (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size()))
                        ? app_documents[active_doc_idx].id : 0;
                    s_depth_target_erase_step = cur_erase_step;
                    s_depth_target_path = active_path;
                    bool triggered = DepthManager::get().trigger_depth_estimate(
                        active_path,
                        depth_far,
                        false,
                        depth_enc
                    );
                    if (triggered) {
                        s_was_depth_processing = true;
                        custom_ui.set_depth_status_text("Running Depth-Anything-V2 (" + depth_enc + ")...", true);
                    } else {
                        std::string err = DepthManager::get().get_last_error();
                        custom_ui.set_depth_status_text(err.empty() ? "Failed to trigger depth estimation." : err, false);
                        if (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size())) {
                            app_documents[active_doc_idx].depth_status_text = custom_ui.get_depth_status_text();
                        }
                    }
                }
            }

            if (DepthManager::get().is_processing()) {
                s_was_depth_processing = true;
                bool on_depth_doc = (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size()) &&
                    app_documents[active_doc_idx].id == s_depth_target_doc_id);
                if (on_depth_doc) {
                    std::string dst = DepthManager::get().get_status();
                    if (!dst.empty()) custom_ui.set_depth_status_text(dst, true);
                } else {
                    std::string saved_st = (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size()))
                        ? app_documents[active_doc_idx].depth_status_text : "";
                    custom_ui.set_depth_status_text(saved_st, false);
                }
            } else if (s_was_depth_processing) {
                s_was_depth_processing = false;
                int target_doc_idx = (s_depth_target_doc_id != 0) ? find_doc_by_id(s_depth_target_doc_id) : active_doc_idx;
                bool on_target_doc = (target_doc_idx >= 0 && target_doc_idx == active_doc_idx);
                std::string err = DepthManager::get().get_last_error();
                if (!err.empty()) {
                    if (target_doc_idx >= 0 && target_doc_idx < static_cast<int>(app_documents.size())) {
                        app_documents[target_doc_idx].depth_status_text = err;
                    }
                    if (on_target_doc) {
                        custom_ui.set_depth_status_text(err, false);
                    }
                } else if (target_doc_idx >= 0 && target_doc_idx < static_cast<int>(app_documents.size()) &&
                           !s_depth_target_path.empty() && fs::exists(s_depth_target_path)) {
                    LoadedImage depth_reloaded;
                    if (ImageLoader::load_from_file(gfx.get_device(), s_depth_target_path.c_str(), depth_reloaded) &&
                        depth_reloaded.has_depth && depth_reloaded.depth_srv) {
                        auto apply_depth = [&](LoadedImage &img) {
                            img.has_depth = depth_reloaded.has_depth;
                            img.depth_srv = depth_reloaded.depth_srv;
                            img.depth_texture = depth_reloaded.depth_texture;
                            img.depth_width = depth_reloaded.depth_width;
                            img.depth_height = depth_reloaded.depth_height;
                            img.is_flat = depth_reloaded.is_flat;
                            img.min_depth = depth_reloaded.min_depth;
                            img.max_depth = depth_reloaded.max_depth;
                            img.depth_pixels = std::move(depth_reloaded.depth_pixels);
                            img.far_plane = depth_reloaded.far_plane;
                        };
                        auto &tdoc = app_documents[target_doc_idx];
                        apply_depth(tdoc.image);
                        tdoc.depth_status_text.clear();
                        if (s_depth_target_erase_step > 0) {
                            fs::path sc_p = s_depth_target_path;
                            sc_p.replace_extension(L".sldepth");
                            if (fs::exists(sc_p) && s_depth_target_erase_step < tdoc.erase_state.stages.size()) {
                                tdoc.erase_state.stages[s_depth_target_erase_step].sidecar = sc_p.wstring();
                            }
                        }
                        if (on_target_doc) {
                            active_image = tdoc.image;
                            sync_depth_to_ipc();
                            custom_ui.set_depth_status(active_image.has_depth, active_image.depth_width, active_image.depth_height, active_image.far_plane);
                            custom_ui.set_depth_status_text("", false);
                        }
                        sync_tabs_to_ui();
                    } else {
                        app_documents[target_doc_idx].depth_status_text = "Depth generated, but sidecar failed to load.";
                        if (on_target_doc) {
                            custom_ui.set_depth_status_text("Depth generated, but sidecar failed to load.", false);
                        }
                    }
                } else if (on_target_doc) {
                    custom_ui.set_depth_status_text("", false);
                }
            }

            // Apply completed EraseTool stage transitions (from LaMa daemon/CLI worker or Undo/Redo Erase)
            {
                std::vector<StageEvent> stage_events;
                {
                    std::lock_guard<std::mutex> lock(g_pending_stage.mtx);
                    if (!g_pending_stage.queue.empty()) {
                        stage_events = std::move(g_pending_stage.queue);
                        g_pending_stage.queue.clear();
                    }
                }

                for (const auto &ev : stage_events) {
                    if (ev.img_path.empty()) continue;
                    int target_doc_idx = ev.dir_key.empty() ? active_doc_idx : find_doc_by_dir_key(ev.dir_key);
                    if (target_doc_idx < 0 || target_doc_idx >= static_cast<int>(app_documents.size())) {
                        if (!ev.dir_key.empty()) {
                            EraseTool::remove_stage_cache_dir(ev.dir_key);
                        }
                        continue;
                    }

                    auto &tdoc = app_documents[target_doc_idx];
                    bool is_active_doc = (target_doc_idx == active_doc_idx);
                    LoadedImage stage_img;
                    if (ImageLoader::load_from_file(gfx.get_device(), ev.img_path.c_str(), stage_img) && stage_img.srv) {
                        if (!stage_img.has_depth && tdoc.image.has_depth && tdoc.image.depth_srv) {
                            stage_img.has_depth = tdoc.image.has_depth;
                            stage_img.depth_srv = tdoc.image.depth_srv;
                            stage_img.depth_texture = tdoc.image.depth_texture;
                            stage_img.depth_width = tdoc.image.depth_width;
                            stage_img.depth_height = tdoc.image.depth_height;
                            stage_img.is_flat = tdoc.image.is_flat;
                            stage_img.min_depth = tdoc.image.min_depth;
                            stage_img.max_depth = tdoc.image.max_depth;
                            stage_img.depth_pixels = tdoc.image.depth_pixels;
                            stage_img.far_plane = tdoc.image.far_plane;
                        }
                        tdoc.image = stage_img;
                        tdoc.erase_strokes.clear();
                        tdoc.erase_undo_stack.clear();
                        tdoc.erase_redo_stack.clear();
                        tdoc.erase_step = static_cast<uint32_t>(ev.step);
                        tdoc.erase_count = static_cast<uint32_t>(ev.total);
                        tdoc.is_dirty = (ev.step > 0);
                        tdoc.inpaint_status_text.clear();

                        if (is_active_doc) {
                            active_image = stage_img;
                            custom_ui.clear_erase_strokes();
                            custom_ui.set_erase_strokes({}, {}, {});
                            EraseTool::get().save_document_state(tdoc.erase_state);
                            if (control_block) {
                                control_block->erase_history_step = static_cast<uint32_t>(ev.step);
                                control_block->erase_history_count = static_cast<uint32_t>(ev.total);
                            }
                            sync_depth_to_ipc();
                            custom_ui.set_depth_status(active_image.has_depth, active_image.depth_width, active_image.depth_height, active_image.far_plane);
                            custom_ui.set_inpaint_status_text("", false);
                        } else {
                            tdoc.erase_state.mask.clear();
                            tdoc.erase_state.mask_white_px = 0;
                            tdoc.erase_state.undo_stack.clear();
                            tdoc.erase_state.redo_stack.clear();
                            tdoc.erase_state.last_error.clear();
                            tdoc.erase_state.stages.resize(ev.step + 1);
                            tdoc.erase_state.stages[ev.step] = { ev.img_path, ev.sidecar_path };
                            tdoc.erase_state.stage_idx = ev.step;
                            tdoc.erase_count = tdoc.erase_state.stages.empty() ? 0u : static_cast<uint32_t>(tdoc.erase_state.stages.size() - 1);
                        }
                        sync_tabs_to_ui();
                        std::cout << "[Inpaint] Reloaded stage " << ev.step << "/" << ev.total << " (" << wide_to_utf8(ev.dir_key.c_str()) << "): " << wide_to_utf8(ev.img_path) << std::endl;
                    } else {
                        std::string load_err = wide_to_utf8(stage_img.error_msg);
                        if (load_err.empty()) load_err = "Failed to load inpainting stage texture.";
                        std::cerr << "[Inpaint] ERROR: " << load_err << std::endl;
                        tdoc.inpaint_status_text = "Failed to load inpainting result: " + load_err;
                        if (is_active_doc) {
                            custom_ui.set_inpaint_status_text(tdoc.inpaint_status_text, false);
                        }
                    }
                }
            }

            if (EraseTool::get().is_processing()) {
                s_was_erase_processing = true;
                std::wstring proc_key = EraseTool::get().get_processing_dir_key();
                std::wstring active_key = (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size()))
                    ? app_documents[active_doc_idx].erase_state.stage_dir_key : L"";
                if (proc_key.empty() || _wcsicmp(proc_key.c_str(), active_key.c_str()) == 0) {
                    std::string st = EraseTool::get().get_status();
                    custom_ui.set_inpaint_status_text(st.empty() ? "Inpainting with LaMa Big HD..." : st, true);
                } else {
                    std::string saved_st = (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size()))
                        ? app_documents[active_doc_idx].inpaint_status_text : "";
                    custom_ui.set_inpaint_status_text(saved_st, false);
                }
            } else if (s_was_erase_processing) {
                s_was_erase_processing = false;
                int target_doc_idx = (s_erase_target_doc_id != 0) ? find_doc_by_id(s_erase_target_doc_id) : active_doc_idx;
                bool on_target_doc = (target_doc_idx >= 0 && target_doc_idx == active_doc_idx);
                std::string err = EraseTool::get().get_last_error();
                if (!err.empty()) {
                    if (target_doc_idx >= 0 && target_doc_idx < static_cast<int>(app_documents.size())) {
                        app_documents[target_doc_idx].inpaint_status_text = "Failed: " + err;
                        app_documents[target_doc_idx].erase_state.last_error = err;
                    }
                    if (on_target_doc) {
                        custom_ui.set_inpaint_status_text("Failed: " + err, false);
                    }
                } else if (on_target_doc) {
                    custom_ui.set_inpaint_status_text("", false);
                }
            }

            std::vector<std::vector<ImVec2>> inpaint_strokes;
            float inpaint_rad = 24.0f;
            if (custom_ui.pop_inpaint_request(inpaint_strokes, inpaint_rad)) {
                auto &erase_tool = EraseTool::get();
                if (erase_tool.is_processing()) {
                    custom_ui.set_inpaint_status_text("Inpainting already in progress...", true);
                } else if (!active_image.texture || active_image.width == 0 || active_image.height == 0) {
                    custom_ui.set_inpaint_status_text("No active image to inpaint.", false);
                } else {
                    if (!erase_tool.has_mask_edits() && !inpaint_strokes.empty()) {
                        custom_ui.sync_erase_strokes_to_tool(active_image.width, active_image.height);
                    }
                    if (!erase_tool.has_mask_edits()) {
                        custom_ui.set_inpaint_status_text("Erase mask is empty. Paint a region first.", false);
                    } else if (!erase_tool.is_model_present()) {
                        custom_ui.set_inpaint_status_text("LaMa model weights not found. Click Download.", false);
                    } else if (erase_tool.find_lama_script_path().empty()) {
                        custom_ui.set_inpaint_status_text("Inpaint script not found: tools/lama/run_lama.py", false);
                    } else {
                        std::wstring doc_key = L"default";
                        if (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size())) {
                            doc_key = app_documents[active_doc_idx].erase_state.stage_dir_key;
                            if (doc_key.empty()) {
                                doc_key = doc_stage_dir_key(app_documents[active_doc_idx].id);
                                app_documents[active_doc_idx].erase_state.stage_dir_key = doc_key;
                            }
                        }
                        erase_tool.set_stage_dir_key(doc_key);

                        fs::path ws_dir = get_workspace_root() / doc_key;
                        std::error_code ec;
                        fs::create_directories(ws_dir, ec);

                        std::wstring active_path = (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size()))
                            ? app_documents[active_doc_idx].file_path : current_image_path;

                        std::wstring tool_base = erase_tool.get_base_image_path();
                        bool needs_tex_export = active_path.empty() || !fs::exists(active_path, ec) ||
                            (control_block && control_block->erase_history_step > 0 && !tool_base.empty() && _wcsicmp(tool_base.c_str(), active_path.c_str()) != 0);
                        if (needs_tex_export) {
                            fs::path base_tmp = ws_dir / L"erase_active_base.png";
                            if (BackbufferDump::save_texture_to_file(gfx.get_device(), gfx.get_context(), active_image.texture.Get(), base_tmp.c_str())) {
                                active_path = base_tmp.wstring();
                            }
                        }

                        fs::path mask_temp = ws_dir / L"erase_mask.png";
                        if (!erase_tool.export_mask_png(mask_temp.wstring(), active_image.width, active_image.height)) {
                            custom_ui.set_inpaint_status_text("Failed to export inpaint mask file.", false);
                        } else if (erase_tool.trigger_erase(active_path, mask_temp.wstring())) {
                            s_was_erase_processing = true;
                            s_erase_target_doc_id = (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size()))
                                ? app_documents[active_doc_idx].id : 0;
                            if (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size())) {
                                erase_tool.save_document_state(app_documents[active_doc_idx].erase_state);
                            }
                            custom_ui.set_inpaint_status_text("Inpainting with LaMa Big HD...", true);
                        } else {
                            std::string err = erase_tool.get_last_error();
                            custom_ui.set_inpaint_status_text(err.empty() ? "Failed to start LaMa inpainting." : err, false);
                        }
                    }
                }
            }

            bool effective_depth_peek = depth_peek || custom_ui.is_depth_peek_active();
            active_image.far_plane = custom_ui.get_depth_far_plane();
            if (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size())) {
                app_documents[active_doc_idx].image.far_plane = active_image.far_plane;
            }

            D3D11_VIEWPORT vp = {};
            vp.TopLeftX = viewable_x;
            vp.TopLeftY = viewable_y;
            vp.Width = viewable_w;
            vp.Height = viewable_h;
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;

            if (effective_depth_peek && active_image.has_depth && active_image.depth_srv) {
                gfx.set_render_target();
                gfx.get_context()->RSSetViewports(1, &vp);
                blit.render_depth_preview(
                    gfx.get_context(),
                    active_image.depth_srv.Get(),
                    viewport.get_transform(),
                    viewable_w,
                    viewable_h,
                    static_cast<float>(active_image.width),
                    static_cast<float>(active_image.height),
                    active_image.far_plane
                );
            } else if (active_image.srv) {
                ID3D11ShaderResourceView *display_srv = nullptr;
                if (custom_ui.is_master_bypass_active()) {
                    display_srv = active_image.srv.Get();
                } else {
                    display_srv = pipeline.process(
                        gfx.get_context(),
                        active_image.srv.Get(),
                        active_image.width,
                        active_image.height,
                        custom_ui.get_effects()
                    );
                }

                gfx.set_render_target();
                gfx.get_context()->RSSetViewports(1, &vp);

                if (custom_ui.is_before_after_active() && scissor_rs) {
                    float split_norm = std::clamp(0.5f + custom_ui.get_before_after_split(), 0.05f, 0.95f);
                    float split_px = viewable_w * split_norm;

                    // 1. Left side (Before / Original)
                    D3D11_RECT scissor_left = {};
                    scissor_left.left = static_cast<LONG>(viewable_x);
                    scissor_left.top = static_cast<LONG>(viewable_y);
                    scissor_left.right = static_cast<LONG>(viewable_x + split_px);
                    scissor_left.bottom = static_cast<LONG>(viewable_y + viewable_h);

                    ID3D11ShaderResourceView *before_srv =
                        (control_block && control_block->erase_history_step > 0 && original_image.srv)
                            ? original_image.srv.Get()
                            : active_image.srv.Get();
                    gfx.get_context()->RSSetState(scissor_rs.Get());
                    gfx.get_context()->RSSetScissorRects(1, &scissor_left);
                    blit.render_transformed(
                        gfx.get_context(),
                        before_srv,
                        viewport.get_transform(),
                        viewable_w,
                        viewable_h,
                        static_cast<float>(active_image.width),
                        static_cast<float>(active_image.height)
                    );

                    // 2. Right side (After / Processed)
                    D3D11_RECT scissor_right = {};
                    scissor_right.left = static_cast<LONG>(viewable_x + split_px);
                    scissor_right.top = static_cast<LONG>(viewable_y);
                    scissor_right.right = static_cast<LONG>(viewable_x + viewable_w);
                    scissor_right.bottom = static_cast<LONG>(viewable_y + viewable_h);

                    gfx.get_context()->RSSetScissorRects(1, &scissor_right);
                    blit.render_transformed(
                        gfx.get_context(),
                        display_srv,
                        viewport.get_transform(),
                        viewable_w,
                        viewable_h,
                        static_cast<float>(active_image.width),
                        static_cast<float>(active_image.height)
                    );

                    gfx.get_context()->RSSetState(nullptr);
                } else {
                    blit.render_transformed(
                        gfx.get_context(),
                        display_srv,
                        viewport.get_transform(),
                        viewable_w,
                        viewable_h,
                        static_cast<float>(active_image.width),
                        static_cast<float>(active_image.height)
                    );
                }
            }

            gfx.reset_viewport();

            if (active_doc_idx >= 0 && active_doc_idx < static_cast<int>(app_documents.size())) {
                bool has_mask = !custom_ui.get_erase_strokes().empty() || EraseTool::get().has_mask_edits();
                if (has_mask && !app_documents[active_doc_idx].is_dirty) {
                    app_documents[active_doc_idx].is_dirty = true;
                    sync_tabs_to_ui();
                }
            }

            custom_ui.begin_frame();
            custom_ui.render(
                gfx.get_render_width(),
                gfx.get_render_height(),
                active_image.width,
                active_image.height,
                viewport.get_transform().zoom,
                viewport.get_transform().pan_x,
                viewport.get_transform().pan_y
            );
            custom_ui.end_frame();

            if (s_pending_ipc_snapshot) {
                s_pending_ipc_snapshot = false;
                bool ok = BackbufferDump::capture_to_file(gfx, s_pending_snapshot_path, gfx.get_render_width(), gfx.get_render_height());
                control_block->mailbox.status = ok ? 1 : -1;
                if (ok) {
                    swprintf_s(control_block->mailbox.response_text, L"Snapshot captured: %ux%u", gfx.get_render_width(), gfx.get_render_height());
                } else {
                    swprintf_s(control_block->mailbox.response_text, L"Failed to capture snapshot");
                }
                MemoryBarrier();
                control_block->mailbox.seq_handled = control_block->mailbox.seq_request;
            }

            // sync interval: only allow unsynced present during active export,
            // keep vsync on in live preview so gpu doesnt melt at 1000+ fps
            UINT sync = 1;
            if (control_block && control_block->export_state != ExportState::Idle && (control_block->export_flags & EXPORT_FLAG_UNSYNCED)) {
                sync = 0;
            }
            gfx.present(sync, 0);
            break;
        }
        }
    }

    EraseTool::get().set_stage_callback(nullptr);
    EraseTool::get().stop_worker();
    pipeline.shutdown();
    custom_ui.shutdown();
    std::wcout << L"[Host] Shutting down host application.\n";
    control_block->host_flags = 0;
    UnmapViewOfFile(control_block);
    CloseHandle(hMap);
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    return run_app(argc, argv);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int ret = run_app(argc, argv);
    if (argv) {
        LocalFree(argv);
    }
    return ret;
}
