#include "cli_test_shader.h"
#include "cli_render.h"
#include "cli_reshade_utils.h"
#include "../gfx_device.h"
#include "../blit_renderer.h"
#include "../../common/ipc_protocol.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

using str_utils::wide_to_utf8;
using str_utils::utf8_to_wide;

int CliTestShader::execute(const CliOptions &opts) {
    if (opts.shader_path.empty()) {
        std::cerr << "Error: No shader file specified for 'test-shader'.\n";
        std::cerr << "Usage: ShaderLab.exe test-shader --shader <path/to/effect.fx> [--json]\n";
        return 1;
    }

    if (!fs::exists(opts.shader_path)) {
        std::cerr << "Error: Shader file not found: " << wide_to_utf8(opts.shader_path.c_str()) << "\n";
        return 1;
    }

    fs::path shader_abs = fs::absolute(opts.shader_path);
    std::string shader_u8 = wide_to_utf8(shader_abs.wstring().c_str());
    std::string filename = shader_abs.filename().string();
    fs::path extra_shader_dir = shader_abs.parent_path();

    if (!opts.quiet && !opts.json_output) {
        std::cout << "========================================\n"
                  << "  ShaderLab ReShade FX Compiler Validator\n"
                  << "========================================\n"
                  << "Testing Shader: " << shader_u8 << "\n\n";
    }

    // synthesize temporary preset for this shader so ReShade compiles and runs it
    fs::path temp_preset = synthesize_shader_preset(opts.shader_path, opts.technique_name, opts.uniform_overrides);
    struct TempPresetGuard {
        fs::path file;
        ~TempPresetGuard() {
            if (!file.empty()) {
                std::error_code ec;
                fs::remove(file, ec);
            }
        }
    } preset_guard{temp_preset};

    fs::path ini_path = find_reshade_ini_path();
    ReShadeIniGuard ini_guard(ini_path);
    configure_reshade_ini(ini_path, temp_preset, extra_shader_dir);

    // clear previous log so we only inspect current invocation
    clear_reshade_log_files(shader_abs);

    wchar_t shm_name[kMaxPathW] = {};
    make_shared_mem_name(shm_name, _countof(shm_name));

    HANDLE hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(SharedControlBlock),
        shm_name
    );

    if (!hMap) {
        std::cerr << "Error: Failed to create IPC file mapping\n";
        return 1;
    }

    auto *control_block = static_cast<SharedControlBlock *>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedControlBlock))
    );

    if (!control_block) {
        std::cerr << "Error: Failed to map IPC control block\n";
        CloseHandle(hMap);
        return 1;
    }

    ZeroMemory(control_block, sizeof(SharedControlBlock));
    control_block->magic = kIpcMagic;
    control_block->version = kIpcVersion;
    control_block->host_flags = HOST_FLAG_ALIVE;
    control_block->export_state = ExportState::Idle;

    GfxDevice gfx;
    if (!gfx.initialize(L"ShaderLab - Shader Validator", 640, 360, 640, 360, false)) {
        std::cerr << "Error: Failed to initialize D3D11 graphics device\n";
        UnmapViewOfFile(control_block);
        CloseHandle(hMap);
        return 1;
    }

    BlitRenderer blit;
    if (!blit.initialize(gfx.get_device())) {
        std::cerr << "Error: Failed to initialize BlitRenderer\n";
        UnmapViewOfFile(control_block);
        CloseHandle(hMap);
        return 1;
    }

    // pump frames while monitoring ReShade.log
    MSG msg = {};
    auto start_time = std::chrono::steady_clock::now();
    int frame_count = 0;
    bool compile_resolved = false;
    std::string final_log_content;

    while (frame_count < 60) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        const float clear_color[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
        gfx.clear(clear_color);
        gfx.present(0, 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        ++frame_count;

        final_log_content = read_reshade_log(shader_abs);
        if (!final_log_content.empty()) {
            bool failed = false, succeeded = false;
            std::string err_text;
            if (check_shader_compile_in_log(final_log_content, filename, failed, succeeded, err_text)) {
                if (failed || (succeeded && frame_count >= 10)) {
                    compile_resolved = true;
                    break;
                }
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() >= 5) {
            break;
        }
    }

    if (final_log_content.empty()) {
        final_log_content = read_reshade_log(shader_abs);
    }

    std::vector<ShaderErrorInfo> errors = parse_reshade_log_errors(final_log_content, filename);

    bool has_compile_errors = false;
    for (const auto &e : errors) {
        if (!e.is_warning) {
            has_compile_errors = true;
            break;
        }
    }

    bool failed_logged = (final_log_content.find("Failed to compile") != std::string::npos &&
                          final_log_content.find(filename) != std::string::npos);

    bool success_logged = (final_log_content.find("Successfully compiled") != std::string::npos &&
                           final_log_content.find(filename) != std::string::npos);

    bool final_compiled = success_logged && !has_compile_errors && !failed_logged;

    gfx.shutdown();
    ini_guard.restore();
    UnmapViewOfFile(control_block);
    CloseHandle(hMap);

    if (opts.json_output) {
        std::cout << "{\n"
                  << "  \"shader\": \"" << json_escape(shader_u8) << "\",\n"
                  << "  \"compiled\": " << (final_compiled ? "true" : "false") << ",\n"
                  << "  \"error_count\": " << errors.size() << ",\n"
                  << "  \"errors\": [\n";
        for (size_t i = 0; i < errors.size(); ++i) {
            const auto &e = errors[i];
            std::cout << "    {\n"
                      << "      \"file\": \"" << json_escape(e.file) << "\",\n"
                      << "      \"line\": " << e.line << ",\n"
                      << "      \"column\": " << e.column << ",\n"
                      << "      \"severity\": \"" << (e.is_warning ? "warning" : "error") << "\",\n"
                      << "      \"code\": \"" << json_escape(e.code) << "\",\n"
                      << "      \"message\": \"" << json_escape(e.message) << "\"\n"
                      << "    }" << (i + 1 < errors.size() ? ",\n" : "\n");
        }
        std::cout << "  ]\n}\n";
    } else {
        if (final_compiled) {
            std::cout << "[SUCCESS] Shader compiled cleanly with zero errors!\n";
        } else {
            std::cout << "[FAILED] Shader compilation failed with " << errors.size() << " issue(s):\n\n";
            for (const auto &e : errors) {
                std::cout << "  " << e.file << "(" << e.line << "," << e.column << "): "
                          << (e.is_warning ? "[WARNING] " : "[ERROR] ")
                          << e.code << ": " << e.message << "\n";
            }
            std::cout << "\n========================================\n";
        }
    }

    if (final_compiled && !opts.input_path.empty()) {
        if (!opts.quiet && !opts.json_output) {
            std::cout << "\n[Rendering Visual Picture Output]\n";
        }
        return CliRender::execute(opts);
    }

    return final_compiled ? 0 : 1;
}
