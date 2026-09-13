#include "cli_test_shader.h"
#include "cli_render.h"
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
#include <regex>

namespace fs = std::filesystem;

#include "../../../common/str_utils.h"

using str_utils::wide_to_utf8;
using str_utils::utf8_to_wide;

struct ShaderErrorInfo {
    std::string file;
    int line = 0;
    int column = 0;
    std::string code;
    std::string message;
    bool is_warning = false;
};

static std::vector<ShaderErrorInfo> parse_reshade_log_errors(const std::string &log_content, const std::string &target_filename) {
    std::vector<ShaderErrorInfo> errors;
    std::stringstream ss(log_content);
    std::string line;

    std::string target_lower = target_filename;
    std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(), [](unsigned char c) { return (char)::tolower(c); });

    std::regex re_err(R"((.+?)\((\d+)(?:,\s*(\d+))?\):\s*(error|warning)\s*([A-Za-z0-9]+)?:\s*(.+))", std::regex::icase);

    while (std::getline(ss, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re_err)) {
            std::string file_in_log = m[1].str();
            std::string file_lower = file_in_log;
            std::transform(file_lower.begin(), file_lower.end(), file_lower.begin(), [](unsigned char c) { return (char)::tolower(c); });

            if (!target_lower.empty() && file_lower.find(target_lower) == std::string::npos) {
                continue;
            }

            std::string l_str = m[2].str();
            std::string c_str = m[3].matched ? m[3].str() : "0";
            std::string severity = m[4].str();
            std::string code = m[5].matched ? m[5].str() : "";
            std::string msg = m[6].str();

            std::transform(severity.begin(), severity.end(), severity.begin(), [](unsigned char c) { return (char)::tolower(c); });

            ShaderErrorInfo info;
            info.file = file_in_log;
            info.line = std::stoi(l_str);
            info.column = std::stoi(c_str);
            info.code = code;
            info.message = msg;
            info.is_warning = (severity == "warning");

            errors.push_back(info);
        }
    }
    return errors;
}

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

    if (!opts.quiet && !opts.json_output) {
        std::cout << "========================================\n"
                  << "  ShaderLab ReShade FX Compiler Validator\n"
                  << "========================================\n"
                  << "Testing Shader: " << shader_u8 << "\n\n";
    }

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

    // clear previous log so we only inspect current invocation
    wchar_t exe_path_w[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path_w, MAX_PATH);
    std::error_code ec;
    fs::remove(fs::current_path() / "ReShade.log", ec);
    fs::remove(fs::path(exe_path_w).parent_path() / "ReShade.log", ec);

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

    // pump few frames so reshade compiles on d3d11 device
    MSG msg = {};
    auto start_time = std::chrono::high_resolution_clock::now();
    int frame_count = 0;

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
    }

    // parse reshade.log
    std::vector<fs::path> log_candidates = {
        fs::current_path() / "ReShade.log",
        fs::path(wide_to_utf8(opts.shader_path.c_str())).parent_path() / "ReShade.log"
    };

    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        log_candidates.push_back(fs::path(exe_path).parent_path() / "ReShade.log");
    }

    std::string log_content;
    for (const auto &lp : log_candidates) {
        if (fs::exists(lp)) {
            std::ifstream in(lp);
            if (in.is_open()) {
                std::stringstream buffer;
                buffer << in.rdbuf();
                log_content = buffer.str();
                if (!log_content.empty()) break;
            }
        }
    }

    std::vector<ShaderErrorInfo> errors = parse_reshade_log_errors(log_content, filename);

    bool has_compile_errors = false;
    for (const auto &e : errors) {
        if (!e.is_warning) {
            has_compile_errors = true;
            break;
        }
    }

    // check if compiled cleanly
    bool failed_logged = (log_content.find("Failed to compile") != std::string::npos && log_content.find(filename) != std::string::npos);

    bool success_logged = (log_content.find("Successfully compiled") != std::string::npos && log_content.find(filename) != std::string::npos);

    bool final_compiled = success_logged && !has_compile_errors && !failed_logged;

    gfx.shutdown();
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
        return final_compiled ? 0 : 1;
    }

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

    if (final_compiled && !opts.input_path.empty()) {
        if (!opts.quiet && !opts.json_output) {
            std::cout << "\n[Rendering Visual Picture Output]\n";
        }
        return CliRender::execute(opts);
    }

    return final_compiled ? 0 : 1;
}
