#include "gfx_device.h"
#include "image_loader.h"
#include "blit_renderer.h"
#include "backbuffer_dump.h"
#include "warning_overlay.h"
#include "settle_detector.h"
#include "hud_renderer.h"
#include "viewport_controller.h"
#include "../../common/ipc_protocol.h"
#include "../../common/depth_chunk.h"
#include "../../common/depth_file.h"
#include "../../common/project_file.h"
#include <shellapi.h>
#include <iostream>
#include <vector>
#include <string>

static std::string wide_to_utf8(const std::wstring &wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}
#include <algorithm>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

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

static std::wstring prepare_working_copy(const std::wstring &source_path) {
    if (source_path.empty() || !fs::exists(source_path) || is_erase_stage_path(source_path)) {
        return source_path;
    }

    wchar_t temp_dir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp_dir) == 0) {
        return source_path;
    }

    fs::path ws_dir = fs::path(temp_dir) / "ShaderLab" / "workspace";
    std::error_code ec;
    fs::create_directories(ws_dir, ec);

    fs::path src(source_path);
    if (path_is_within(src, ws_dir)) {
        return source_path;
    }

    fs::path dst = ws_dir / src.filename();

    // clean previous workspace contents so stale sidecars or leftover files dont stick around
    for (const auto &entry : fs::directory_iterator(ws_dir, ec)) {
        if (entry.path().filename() != L"erase_stages") {
            fs::remove_all(entry.path(), ec);
        }
    }

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


static bool on_warning_input_hook(void *user_data, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *overlay = static_cast<WarningOverlay *>(user_data);
    if (!overlay || !overlay->is_active()) return false;

    switch (msg) {
    case WM_MOUSEMOVE: {
        int x = static_cast<short>(LOWORD(lParam));
        int y = static_cast<short>(HIWORD(lParam));
        overlay->on_mouse_move(x, y);
        return false;
    }
    case WM_LBUTTONDOWN: {
        int x = static_cast<short>(LOWORD(lParam));
        int y = static_cast<short>(HIWORD(lParam));
        return overlay->on_mouse_click(x, y);
    }
    case WM_KEYDOWN: {
        if (wParam == VK_F11) return false; // let f11 pass through even if warning active
        overlay->on_key_down(wParam);
        return true;
    }
    }
    return false;
}

struct ViewportHookContext {
    ViewportController *controller = nullptr;
    GfxDevice *gfx = nullptr;
    LoadedImage *active_image = nullptr;
    SharedControlBlock *block = nullptr;
    ToastHud *toast_hud = nullptr;
};

static uint64_t g_last_fullscreen_toggle_ms = 0;
static void toggle_fullscreen_safe(GfxDevice &gfx, ToastHud *toast, SharedControlBlock *block) {
    uint64_t now_ms = GetTickCount64();
    if (now_ms - g_last_fullscreen_toggle_ms < 300) return;
    g_last_fullscreen_toggle_ms = now_ms;

    gfx.toggle_fullscreen();
    if (toast) {
        toast->show(gfx.is_fullscreen() ? L"Borderless Fullscreen" : L"Windowed");
    }
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
                toggle_fullscreen_safe(*ctx->gfx, ctx->toast_hud, ctx->block);
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

struct GdiplusScope {
    ULONG_PTR token = 0;
    GdiplusScope() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
    }
    ~GdiplusScope() {
        if (token) {
            Gdiplus::GdiplusShutdown(token);
            token = 0;
        }
    }
};

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
    GdiplusScope gdiplus_scope;

    // set up the shared block first, before any gpu stuff, so the addon can latch onto it asap
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

    LoadedImage active_image;
    LoadedImage original_image;
    std::wstring display_image_name;
    ViewportController viewport;
    SettleDetector settle_detector;
    CompassHud compass_hud;
    ZoomHud zoom_hud;
    ToastHud toast_hud;
    WelcomeHud welcome_hud;

    HudFontManager::get().initialize();

    compass_hud.initialize(gfx.get_device());
    zoom_hud.initialize(gfx.get_device());
    toast_hud.initialize(gfx.get_device());
    welcome_hud.initialize(gfx.get_device());

    // if reshade is missing, show a big "go install it" overlay instead of just... nothing
    bool has_reshade = fs::exists(L"dxgi.dll") || fs::exists(L"ReShade64.dll");
    if (has_reshade) {
        control_block->host_flags |= HOST_FLAG_EXTERNAL_HUD;
    }

    WarningOverlay warning;
    if (warning.initialize(gfx.get_device())) {
        // check both naming conventions, reshade setup can go either way
        if (!has_reshade) {
            warning.set_active(true);
        }
    }

    // input hooks: warning overlay goes first since it's modal, then ViewportController
    gfx.add_input_hook(on_warning_input_hook, &warning);

    ViewportHookContext vp_ctx;
    vp_ctx.controller = &viewport;
    vp_ctx.gfx = &gfx;
    vp_ctx.active_image = &active_image;
    vp_ctx.block = control_block;
    vp_ctx.toast_hud = &toast_hud;
    gfx.add_input_hook(on_viewport_input_hook, &vp_ctx);

    std::wcout << L"[Host] Graphics device initialized. Entering live render loop.\n";

    uint32_t last_preview_counter = 0;
    uint32_t last_synced_xform_ver = 0;
    uint32_t last_request_toggle_fullscreen = 0;
    uint32_t last_lock_flags = control_block->view_interaction_flags & (VIEW_FLAG_LOCK_PAN | VIEW_FLAG_LOCK_ZOOM | VIEW_FLAG_LOCK_ROT);
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

    // external hud composite tracking
    uint32_t last_addon_heartbeat = 0;
    auto last_addon_beat_time = std::chrono::steady_clock::now();
    bool addon_alive = false;

    auto publish_hud = [&](const uint64_t (&handles)[kHudLayers]) {
        bool changed = false;
        for (uint32_t i = 0; i < kHudLayers; ++i) {
            if (control_block->hud_srv[i] != handles[i]) {
                control_block->hud_srv[i] = handles[i];
                changed = true;
            }
        }
        if (changed) control_block->hud_version++;
    };

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

    // check if launched with an image or project file argument
    std::wstring launch_path;
    if (argc > 1 && argv[1] && argv[1][0] != L'-') {
        std::wstring raw_arg = argv[1];
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
            launch_path = raw_arg;
        }
    }

    if (!launch_path.empty()) {
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

    bool running = true;
    const float idle_color[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
    auto last_time = std::chrono::steady_clock::now();

    // first-run nudge: addon flags this in shared memory on the very first launch,
    // host resizes the swapchain by a couple pixels and back to force reshade to
    // reload and apply the InputProcessing setting right away
    int nudge_stage = 0; // 0 = idle, 1 = shrunk (restore next frame)

    while (running) {
        if (control_block) {
            viewport.set_erase_active((control_block->view_interaction_flags & VIEW_FLAG_ERASE_ACTIVE) != 0);
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
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!running) break;

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        dt = (std::min)(dt, 0.1f); // cap dt so a hiccup doesnt send the fade animation flying

        // first-run nudge, driven by the addon's flag in shared memory
        if (control_block->first_run_nudge != 0 && nudge_stage == 0) {
            gfx.resize_buffers(gfx.get_render_width() - 4, gfx.get_render_height() - 4);
            nudge_stage = 1;
        } else if (nudge_stage == 1) {
            gfx.resize_buffers(gfx.get_render_width() + 4, gfx.get_render_height() + 4);
            nudge_stage = 0;
            control_block->first_run_nudge = 0;
        }

        control_block->heartbeat++;

        // keep the fine-tune key synced with whatever the addon has configured
        viewport.set_fine_vk(control_block->fine_tune_vk);

        // poll f11 directly as fallback if reshade swallowed the keydown event
        static bool s_f11_prev = false;
        bool f11_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11_down && !s_f11_prev) {
            DWORD fg_pid = 0;
            HWND fg = GetForegroundWindow();
            if (fg) GetWindowThreadProcessId(fg, &fg_pid);
            if (fg_pid == GetCurrentProcessId()) {
                toggle_fullscreen_safe(gfx, &toast_hud, control_block);
            }
        }
        s_f11_prev = f11_down;

        // track addon liveness so we can fall back to local HUD compositing if the
        // addon somehow failed to load even with reshade present
        if (control_block->addon_heartbeat != last_addon_heartbeat) {
            last_addon_heartbeat = control_block->addon_heartbeat;
            last_addon_beat_time = now;
            addon_alive = true;
        } else if (std::chrono::duration<float>(now - last_addon_beat_time).count() > 2.0f) {
            addon_alive = false;
        }

        const bool external_hud = has_reshade && addon_alive && (control_block->effects_enabled != 0);

        // render the HUD layers: either published for external composite after reshade
        // effects, or blended locally if reshade effects are off or addon not connected
        auto render_hud_layers = [&]() {
            float cx = 0.0f, cy = 0.0f, sw = 0.0f, sh = 0.0f;
            viewport.get_screen_image_geometry(
                gfx.get_render_width(), gfx.get_render_height(),
                active_image.width, active_image.height,
                cx, cy, sw, sh
            );

            uint64_t now_ms = GetTickCount64();
            float idle_rotate_s = static_cast<float>(now_ms - control_block->view_last_rotate_ms) / 1000.0f;
            float idle_zoom_s   = static_cast<float>(now_ms - control_block->view_last_zoom_ms) / 1000.0f;

            bool is_rot_locked = viewport.is_lock_rotate() || ((control_block->view_interaction_flags & VIEW_FLAG_LOCK_ROT) != 0);
            bool is_rotating_or_attempt = viewport.is_rotating() || viewport.is_rotate_locked_attempt() ||
                                          ((control_block->view_interaction_flags & VIEW_FLAG_ROTATING) != 0) ||
                                          (is_rot_locked && idle_rotate_s < 1.0f);

            compass_hud.update(
                dt,
                is_rotating_or_attempt,
                idle_rotate_s,
                viewport.get_snapped_angle(),
                viewport.is_snapped(),
                viewport.is_fine(),
                cx, cy, sw, sh,
                is_rot_locked
            );
            if (compass_hud.is_visible()) {
                compass_hud.render(gfx.get_context(), gfx.get_render_width(), gfx.get_render_height());
                if (!external_hud && compass_hud.get_srv()) {
                    blit.render_blend(gfx.get_context(), compass_hud.get_srv());
                }
            }

            bool is_zoom_locked = viewport.is_lock_zoom() || ((control_block->view_interaction_flags & VIEW_FLAG_LOCK_ZOOM) != 0);
            zoom_hud.update(dt, viewport.get_transform().zoom, idle_zoom_s, is_zoom_locked, viewport.is_fine());
            if (zoom_hud.is_visible()) {
                zoom_hud.render(gfx.get_context(), gfx.get_render_width(), gfx.get_render_height());
                if (!external_hud && zoom_hud.get_srv()) {
                    blit.render_blend(gfx.get_context(), zoom_hud.get_srv());
                }
            }

            DWORD fg_pid = 0;
            HWND fg_hwnd = GetForegroundWindow();
            if (fg_hwnd) GetWindowThreadProcessId(fg_hwnd, &fg_pid);
            bool is_app_focused = (fg_pid == GetCurrentProcessId());
            bool want_text = (control_block->view_interaction_flags & VIEW_FLAG_TEXT_INPUT) != 0;
            bool is_d_pressed = (GetAsyncKeyState('D') & 0x8000) != 0;
            bool depth_peek = active_image.has_depth && active_image.depth_srv && is_d_pressed && is_app_focused && !want_text;
            bool pan_locked_held = (control_block->view_interaction_flags & VIEW_FLAG_PAN_LOCKED_ATTEMPT) != 0 ||
                                   viewport.is_pan_locked_attempt();
            bool is_dragging_ba_pos = viewport.is_before_after_enabled() && viewport.is_dragging_split_pos();
            bool is_dragging_ba_rot = viewport.is_before_after_enabled() && viewport.is_dragging_split_rot();

            if (depth_peek) {
                toast_hud.show_held(L"Depth Peek (Holding D)");
            } else if (pan_locked_held) {
                toast_hud.show_held(L"Pan Locked");
            } else if (is_dragging_ba_pos) {
                int pct = static_cast<int>(std::round((viewport.get_before_after_split() + 0.5f) * 100.0f));
                wchar_t buf[48];
                swprintf_s(buf, L"Split: %d%%", pct);
                toast_hud.show_held(buf);
            } else if (is_dragging_ba_rot) {
                float ang = viewport.get_before_after_angle();
                bool snapped = (std::fmod(ang, 45.0f) == 0.0f);
                wchar_t buf[64];
                if (snapped) {
                    swprintf_s(buf, L"Split Angle: %.0f° (Snapped)", ang);
                } else {
                    swprintf_s(buf, L"Split Angle: %.1f°", ang);
                }
                toast_hud.show_held(buf);
            } else {
                toast_hud.release_held();
            }

            toast_hud.update(dt);
            if (toast_hud.is_visible()) {
                toast_hud.render(gfx.get_context(), gfx.get_render_width(), gfx.get_render_height());
                if (!external_hud && toast_hud.get_srv()) {
                    blit.render_blend(gfx.get_context(), toast_hud.get_srv());
                }
            }

            if (warning.is_active()) {
                warning.update_and_render(gfx.get_context(), gfx.get_render_width(), gfx.get_render_height(), dt);
                if (warning.get_srv()) {
                    blit.render_blend(gfx.get_context(), warning.get_srv());
                }
            }

            if (external_hud) {
                uint64_t handles[kHudLayers] = { 0, 0, 0 };
                if (compass_hud.is_visible()) handles[0] = compass_hud.get_srv_handle();
                if (zoom_hud.is_visible())    handles[1] = zoom_hud.get_srv_handle();
                if (toast_hud.is_visible())   handles[2] = toast_hud.get_srv_handle();
                publish_hud(handles);
            } else {
                uint64_t zero_handles[kHudLayers] = { 0, 0, 0 };
                publish_hud(zero_handles);
            }
        };

        // sync the view transform if the addon modified it, e.g. while the reshade overlay is open
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

        // toggle fullscreen if requested by reshade helper overlay
        if (control_block->request_toggle_fullscreen != last_request_toggle_fullscreen) {
            last_request_toggle_fullscreen = control_block->request_toggle_fullscreen;
            toggle_fullscreen_safe(gfx, &toast_hud, control_block);
        }

        // show toast notification if requested by addon or other components
        static uint32_t s_last_toast_ver = 0;
        if (control_block->toast_version != s_last_toast_ver) {
            s_last_toast_ver = control_block->toast_version;
            if (control_block->toast_message[0] != L'\0') {
                toast_hud.show(control_block->toast_message);
            }
        }

        // detect before/after toggle or lock flag changes to show top notification
        static uint32_t s_last_ba_flag = 0;
        uint32_t cur_ba_flag = control_block->view_interaction_flags & VIEW_FLAG_BEFORE_AFTER;
        static uint32_t s_last_erase_flag = 0;
        uint32_t cur_erase_flag = control_block->view_interaction_flags & VIEW_FLAG_ERASE_ACTIVE;
        uint32_t cur_lock_flags = control_block->view_interaction_flags & (VIEW_FLAG_LOCK_PAN | VIEW_FLAG_LOCK_ZOOM | VIEW_FLAG_LOCK_ROT);
        if (cur_erase_flag != s_last_erase_flag) {
            s_last_erase_flag = cur_erase_flag;
            toast_hud.show(cur_erase_flag ? L"Erase Mode Active" : L"Erase Mode Disabled");
        } else if (cur_ba_flag != s_last_ba_flag) {
            s_last_ba_flag = cur_ba_flag;
            toast_hud.show(cur_ba_flag ? L"Before / After Comparison Active" : L"Before / After Disabled");
            last_lock_flags = cur_lock_flags;
        } else if (cur_lock_flags != last_lock_flags) {
            uint32_t diff = cur_lock_flags ^ last_lock_flags;
            uint32_t all_mask = (VIEW_FLAG_LOCK_PAN | VIEW_FLAG_LOCK_ZOOM | VIEW_FLAG_LOCK_ROT);
            if ((diff & (diff - 1)) != 0) {
                // multiple locks toggled at once (e.g. Lock View / All)
                bool all_on = (cur_lock_flags & all_mask) == all_mask;
                toast_hud.show(all_on ? L"View Locked (All)" : L"View Unlocked");
            } else if (diff & VIEW_FLAG_LOCK_PAN) {
                toast_hud.show((cur_lock_flags & VIEW_FLAG_LOCK_PAN) ? L"Pan Locked" : L"Pan Unlocked");
            } else if (diff & VIEW_FLAG_LOCK_ZOOM) {
                toast_hud.show((cur_lock_flags & VIEW_FLAG_LOCK_ZOOM) ? L"Zoom Locked" : L"Zoom Unlocked");
            } else if (diff & VIEW_FLAG_LOCK_ROT) {
                toast_hud.show((cur_lock_flags & VIEW_FLAG_LOCK_ROT) ? L"Rotation Locked" : L"Rotation Unlocked");
            }
            last_lock_flags = cur_lock_flags;
        }

        // process host-targeted mailbox commands from CLI / MCP / automation
        if (control_block->mailbox.seq_request != control_block->mailbox.seq_handled) {
            uint32_t cmd = control_block->mailbox.cmd_id;
            if (cmd == static_cast<uint32_t>(IpcCmd::Snapshot)) {
                // capture live backbuffer to destination file
                bool ok = BackbufferDump::capture_to_file(gfx, control_block->mailbox.path, gfx.get_render_width(), gfx.get_render_height());
                control_block->mailbox.status = ok ? 1 : -1;
                if (ok) {
                    swprintf_s(control_block->mailbox.response_text, L"Snapshot captured: %ux%u", gfx.get_render_width(), gfx.get_render_height());
                } else {
                    swprintf_s(control_block->mailbox.response_text, L"Failed to capture snapshot");
                }
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
                if (action_id == static_cast<uint32_t>(IpcAction::ResetRotation)) {
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
                    toggle_fullscreen_safe(gfx, &toast_hud, control_block);
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
                    wchar_t temp_dir[MAX_PATH] = {};
                    GetTempPathW(MAX_PATH, temp_dir);
                    fs::path ws_dir = fs::path(temp_dir) / "ShaderLab" / "workspace";
                    std::error_code ec;
                    fs::create_directories(ws_dir, ec);

                    // clean previous workspace contents so stale sidecars or files dont linger
                    for (const auto &entry : fs::directory_iterator(ws_dir, ec)) {
                        fs::remove_all(entry.path(), ec);
                    }

                    std::wstring out_img_path, out_preset_path, load_err;
                    project_file::ProjectManifest manifest;
                    if (project_file::load_project(proj_src_path, ws_dir.wstring(), out_img_path, out_preset_path, manifest, load_err)) {
                        LoadedImage new_preview;
                        if (ImageLoader::load_from_file(gfx.get_device(), out_img_path.c_str(), new_preview)) {
                            active_image = new_preview;
                            original_image = active_image;
                            control_block->erase_history_step = 0;
                            control_block->erase_history_count = 0;
                            display_image_name = fs::path(proj_src_path).filename().wstring();
                            if (manifest.depth.has_depth && manifest.depth.far_plane > 0.0f) {
                                active_image.far_plane = manifest.depth.far_plane;
                            }
                            control_block->view_image_width = active_image.width;
                            control_block->view_image_height = active_image.height;
                            sync_depth_to_ipc();

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
                            set_host_status(control_block, L"Project error: failed to decode image inside " + fs::path(proj_src_path).filename().wstring());
                        }
                    } else {
                        set_host_status(control_block, L"Failed to load project: " + load_err);
                    }
                } else {
                    std::wstring orig_source_path = control_block->preview_path;
                    std::wstring work_path = prepare_working_copy(orig_source_path);
                    if (work_path != control_block->preview_path) {
                        wcsncpy_s(control_block->preview_path, kMaxPathW, work_path.c_str(), _TRUNCATE);
                    }
                    if (orig_source_path != work_path) {
                        wcsncpy_s(control_block->dropped_file_path, kMaxPathW, orig_source_path.c_str(), _TRUNCATE);
                        control_block->dropped_file_counter++;
                    }

                    LoadedImage new_preview;
                    if (ImageLoader::load_from_file(gfx.get_device(), control_block->preview_path, new_preview)) {
                        active_image = new_preview;
                        if (!is_erase_stage_path(control_block->preview_path)) {
                            original_image = active_image;
                            control_block->erase_history_step = 0;
                            control_block->erase_history_count = 0;
                            display_image_name = fs::path(control_block->preview_path).filename().wstring();

                            // reset active project since a raw image was opened
                            control_block->active_project_path[0] = L'\0';
                            control_block->project_dirty = 0;
                            control_block->active_project_version++;
                        }
                        control_block->view_image_width = active_image.width;
                        control_block->view_image_height = active_image.height;
                        sync_depth_to_ipc();

                        std::wstring depth_info = active_image.has_depth ? (L" [Depth Active, F=" + std::to_wstring(static_cast<int>(active_image.far_plane)) + L"]") : L" [2D]";
                        set_host_status(control_block, L"Loaded: " + display_image_name +
                                        L" (" + std::to_wstring(active_image.width) + L"x" + std::to_wstring(active_image.height) + L")" + depth_info);
                        update_window_title(L"ShaderLab - " + display_image_name);
                    }
                }
            }
        }

        sync_depth_to_ipc();

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

            // wait until reshade_finish_effects fires so effects are fully loaded on new canvas.
            // only require effects_ready to advance past the snapshot: once finish_effects has
            // fired at least one frame the runtime is live and effects rendered cleanly.
            // don't gate on effects_compiling == 0: preset sync inside on_reshade_finish_effects
            // can set it back to 1 in the same callback that cleared it, which spins a whole
            // second recompile cycle even though effects were already rendering fine
            bool effects_enabled = has_reshade && addon_alive && (control_block->effects_enabled != 0);
            if (effects_enabled && !export_effects_initialized) {
                bool finish_effects_fired = (control_block->effects_ready > export_start_effects_ready);
                if (!finish_effects_fired) {
                    update_window_title(L"ShaderLab - Compiling ReShade Shaders...");
                    set_host_status(control_block, L"Compiling ReShade shaders across canvas...");
                    export_reload_wait_frames++;
                    // safety timeout 1800 frames (~30s at 60fps) to avoid hanging if compilation fails
                    if (export_reload_wait_frames < 1800) {
                        break; // keep presenting frames so reshade compiles on worker threads
                    }
                }
                export_effects_initialized = true;
                control_block->export_frame_index = 0;
                settle_detector.reset();
            } else if (!effects_enabled && !export_effects_initialized) {
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
                bool effects_active = has_reshade && addon_alive && (control_block->effects_enabled != 0);
                converged = settle_detector.check_convergence(
                    gfx.get_device(),
                    gfx.get_context(),
                    gfx.get_swap_chain(),
                    delta,
                    effects_active
                );
                control_block->export_last_delta = delta;

                // when effects are active (DoF, TAA, Bloom, motion blur), enforce a minimum
                // warmup of 60 frames (1 full second) before allowing early convergence, ensuring
                // slow-settling effects have started accumulating and autofocus has begun
                constexpr uint32_t kMinWarmupFrames = 60;
                if (effects_active && control_block->export_frame_index < kMinWarmupFrames) {
                    converged = false;
                }

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

            // addon already completed or failed export in on_reshade_finish_effects: skip
            // host fallback capture so we never overwrite the addon's output
            if (control_block->export_state == ExportState::Done || control_block->export_state == ExportState::Failed) {
                export_capture_wait_frames = 0;
                gfx.present(1, 0);
                break;
            }

            // addon owns the capture in on_reshade_finish_effects while effects are active
            // and connected; host captures directly only when effects are inactive, or as a
            // safety fallback after 180 frames (~3s at 60fps)
            bool reshade_effects_active = has_reshade && addon_alive && (control_block->effects_enabled != 0);
            if (!reshade_effects_active || export_capture_wait_frames >= 180) {
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

            if (depth_peek && active_image.has_depth && active_image.depth_srv) {
                blit.render_depth_preview(
                    gfx.get_context(),
                    active_image.depth_srv.Get(),
                    viewport.get_transform(),
                    static_cast<float>(gfx.get_render_width()),
                    static_cast<float>(gfx.get_render_height()),
                    static_cast<float>(active_image.width),
                    static_cast<float>(active_image.height),
                    active_image.far_plane
                );
            } else if (active_image.srv) {
                blit.render_transformed(
                    gfx.get_context(),
                    active_image.srv.Get(),
                    viewport.get_transform(),
                    static_cast<float>(gfx.get_render_width()),
                    static_cast<float>(gfx.get_render_height()),
                    static_cast<float>(active_image.width),
                    static_cast<float>(active_image.height)
                );
            }

            // welcome hud when no image loaded
            welcome_hud.update(dt, active_image.srv != nullptr);
            if (welcome_hud.is_visible()) {
                welcome_hud.render(gfx.get_context(), gfx.get_render_width(), gfx.get_render_height());
                if (welcome_hud.get_srv()) {
                    blit.render_blend(gfx.get_context(), welcome_hud.get_srv());
                }
            }

            // hud layers
            render_hud_layers();

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
