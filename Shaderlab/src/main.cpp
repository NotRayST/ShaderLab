#include "gfx_device.h"
#include "image_loader.h"
#include "blit_renderer.h"
#include "backbuffer_dump.h"
#include "warning_overlay.h"
#include "../../common/ipc_protocol.h"
#include <shellapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

static void set_host_status(SharedControlBlock *block, const std::wstring &msg) {
    if (!block) return;
    wcsncpy_s(block->host_status, kMaxPathW, msg.c_str(), _TRUNCATE);
    std::wcout << L"[Host] " << msg << L"\n";
}

static bool on_input_hook(void *user_data, UINT msg, WPARAM wParam, LPARAM lParam) {
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
        overlay->on_key_down(wParam);
        return true;
    }
    }
    return false;
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    // selftest mode: no reshade needed, just exercises gpu + readback so we know the machine works
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--selftest") == 0) {
            const wchar_t *input_path = (i + 1 < argc) ? argv[i + 1] : L"sample.png";
            const wchar_t *output_path = (i + 2 < argc && argv[i + 2][0] != L'-') ? argv[i + 2] : L"_selftest.png";

            std::wcout << L"========================================\n";
            std::wcout << L"  ShaderLab - GPU Self-Test Mode\n";
            std::wcout << L"========================================\n";

            GfxDevice gfx;
            if (!gfx.initialize(L"ShaderLab - GPU Self-Test", 960, 540, 960, 540)) {
                std::wcerr << L"[SelfTest] GfxDevice initialization failed!\n";
                return 1;
            }

            BlitRenderer blit;
            if (!blit.initialize(gfx.get_device())) {
                std::wcerr << L"[SelfTest] BlitRenderer initialization failed!\n";
                return 1;
            }

            bool ok = BackbufferDump::run_selftest(gfx, blit, input_path, output_path);
            return ok ? 0 : 1;
        }
    }

    std::wcout << L"========================================\n";
    std::wcout << L"  ShaderLab - Native Host Application\n";
    std::wcout << L"========================================\n";

    // shared block comes first, before any gpu stuff, so the addon can latch asap
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
    control_block->host_flags = 1;
    control_block->export_state = ExportState::Idle;
    set_host_status(control_block, L"Host initialized. Ready.");

    // d3d11 + swapchain, sized to the work area (not the fixed 960x540 window)
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

    // if reshade is missing show a big "go install it" overlay instead of nothing
    WarningOverlay warning;
    if (warning.initialize(gfx.get_device())) {
        gfx.set_input_hook(on_input_hook, &warning);

        // check both naming conventions since reshade setup can use either (I think, not sure.. but its a small fix so whatever)
        bool has_reshade = fs::exists(L"dxgi.dll") || fs::exists(L"ReShade64.dll");
        if (!has_reshade) {
            warning.set_active(true);
        }
    }

    std::wcout << L"[Host] Graphics device initialized. Entering live render loop.\n";

    LoadedImage active_image;
    uint32_t last_preview_counter = 0;
    uint32_t settle_counter = 0;

    // boot with a sample image on screen so it isnt just a void on first launch
    const wchar_t *startup_candidates[] = { L"sample.png", L"test.png", L"myimage.png", L"test/sample.png" };
    for (const wchar_t *candidate : startup_candidates) {
        if (fs::exists(candidate)) {
            if (ImageLoader::load_from_file(gfx.get_device(), candidate, active_image)) {
                wcsncpy_s(control_block->preview_path, kMaxPathW, candidate, _TRUNCATE);
                control_block->preview_counter = 1;
                last_preview_counter = 1;
                set_host_status(control_block, L"Live Preview: " + std::wstring(candidate) +
                                L" (" + std::to_wstring(active_image.width) + L"x" + std::to_wstring(active_image.height) + L")");
                break;
            }
        }
    }

    bool running = true;
    const float idle_color[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
    auto last_time = std::chrono::steady_clock::now();

    while (running) {
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

        // bump so the addon knows we're still alive. this whole ipc dance took too long to fix
        control_block->heartbeat++;

        // 1. drag & drop directly onto the host window
        std::wstring dropped_file;
        if (gfx.get_and_clear_dropped_file(dropped_file)) {
            if (fs::exists(dropped_file)) {
                wcsncpy_s(control_block->preview_path, kMaxPathW, dropped_file.c_str(), _TRUNCATE);
                control_block->preview_counter++;
            }
        }

        // 2. new preview image requested (button in overlay or drop), (re)load it
        if (control_block->preview_counter != last_preview_counter) {
            last_preview_counter = control_block->preview_counter;
            if (wcslen(control_block->preview_path) > 0 && fs::exists(control_block->preview_path)) {
                LoadedImage new_preview;
                if (ImageLoader::load_from_file(gfx.get_device(), control_block->preview_path, new_preview)) {
                    active_image = new_preview;
                    set_host_status(control_block, L"Loaded: " + std::wstring(control_block->preview_path) +
                                    L" (" + std::to_wstring(active_image.width) + L"x" + std::to_wstring(active_image.height) + L")");
                }
            }
        }

        // 3. export state machine O_O
        switch (control_block->export_state) {
        case ExportState::Requested: {
            control_block->export_state = ExportState::Loading;
            set_host_status(control_block, L"Export: loading image " + std::wstring(control_block->export_input_path));

            if (!ImageLoader::load_from_file(gfx.get_device(), control_block->export_input_path, active_image)) {
                control_block->export_error = IPC_ERR_LOAD_FAILED;
                control_block->export_state = ExportState::Failed;
                set_host_status(control_block, L"Export failed: could not load image");
                break;
            }

            gfx.set_processing_image(true);
            if (!gfx.resize_buffers(active_image.width, active_image.height)) {
                control_block->export_error = IPC_ERR_RESIZE_FAILED;
                control_block->export_state = ExportState::Failed;
                gfx.set_processing_image(false);
                set_host_status(control_block, L"Export failed: swap chain resize failed");
                break;
            }

            settle_counter = 10; // let multi-pass/temporal shaders warm up before we capture. fucking temporal shaders
            // TODO: really should let the user tweak this, needed for the temporal stuff.
            control_block->export_state = ExportState::Rendering;
            break;
        }
        case ExportState::Rendering: {
            gfx.set_render_target();
            gfx.clear(idle_color);
            blit.render(gfx.get_context(), active_image.srv.Get());
            gfx.present(1, 0);

            if (settle_counter > 0) {
                settle_counter--;
            } else {
                control_block->export_state = ExportState::Capturing;
            }
            break;
        }
        case ExportState::Capturing: {
            gfx.set_render_target();
            gfx.clear(idle_color);
            blit.render(gfx.get_context(), active_image.srv.Get());
            gfx.present(1, 0);

            // addon's finish_effects hook fires inside present() and does the actual capture here
            break;
        }
        case ExportState::Done:
        case ExportState::Failed: {
            // reset back to window size & idle so the next export starts clean
            gfx.set_processing_image(false);
            control_block->export_state = ExportState::Idle;

            // keep presenting or the message pump goes silent and the window freezes
            gfx.set_render_target();
            gfx.clear(idle_color);

            if (active_image.srv) {
                gfx.set_viewport_aspect_fit(active_image.width, active_image.height);
                blit.render(gfx.get_context(), active_image.srv.Get());
                gfx.reset_viewport();
            }

            if (warning.is_active()) {
                warning.update_and_render(gfx.get_context(), gfx.get_render_width(), gfx.get_render_height(), dt);
                if (warning.get_srv()) {
                    blit.render_blend(gfx.get_context(), warning.get_srv());
                }
            }

            gfx.present(1, 0);
            break;
        }
        case ExportState::Idle:
        default: {
            // steady-state live view: blit the active image, overlay on top, present
            gfx.set_render_target();
            gfx.clear(idle_color);

            if (active_image.srv) {
                gfx.set_viewport_aspect_fit(active_image.width, active_image.height);
                blit.render(gfx.get_context(), active_image.srv.Get());
                gfx.reset_viewport();
            }

            // reshade-missing overlay
            if (warning.is_active()) {
                warning.update_and_render(gfx.get_context(), gfx.get_render_width(), gfx.get_render_height(), dt);
                if (warning.get_srv()) {
                    blit.render_blend(gfx.get_context(), warning.get_srv());
                }
            }

            gfx.present(1, 0);
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
