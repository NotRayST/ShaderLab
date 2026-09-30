#include "cli_render.h"
#include "cli_reshade_utils.h"
#include "../gfx_device.h"
#include "../image_loader.h"
#include "../blit_renderer.h"
#include "../backbuffer_dump.h"
#include "../settle_detector.h"
#include "../../common/ipc_protocol.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>
#include <thread>
#include <algorithm>
#include <regex>
#include <unordered_map>

namespace fs = std::filesystem;

using str_utils::wide_to_utf8;
using str_utils::utf8_to_wide;

#include "../../third_party/stb/stb_image.h"

static fs::path create_preset_with_overrides(const fs::path &base_preset, const std::string &default_section, const std::vector<std::pair<std::string, std::string>> &uniforms) {
    std::ifstream in(base_preset);
    if (!in.is_open()) return base_preset;

    wchar_t temp_dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp_dir);
    fs::path temp_preset = fs::path(temp_dir) / ("ShaderLab_override_" + std::to_string(GetCurrentProcessId()) + "_" + base_preset.filename().string());

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    in.close();

    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> section_overrides;
    for (const auto &kv : uniforms) {
        std::string sec = default_section;
        std::string var = kv.first;
        size_t colon = kv.first.find(':');
        if (colon != std::string::npos) {
            sec = kv.first.substr(0, colon);
            var = kv.first.substr(colon + 1);
        } else if (sec.empty()) {
            std::string cur_sec;
            for (const auto &l : lines) {
                std::string t = l;
                t.erase(0, t.find_first_not_of(" \t\r\n"));
                t.erase(t.find_last_not_of(" \t\r\n") + 1);
                if (t.rfind("[", 0) == 0 && t.back() == ']') {
                    cur_sec = t.substr(1, t.size() - 2);
                } else if (!cur_sec.empty() && t.rfind(var + "=", 0) == 0) {
                    sec = cur_sec;
                    break;
                }
            }
            if (sec.empty()) {
                sec = base_preset.filename().string();
            }
        }
        section_overrides[sec].push_back({var, kv.second});
    }

    for (const auto &[sec, kvs] : section_overrides) {
        std::string sec_header = "[" + sec + "]";
        bool found_sec = false;

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string t = lines[i];
            t.erase(0, t.find_first_not_of(" \t\r\n"));
            t.erase(t.find_last_not_of(" \t\r\n") + 1);
            if (_stricmp(t.c_str(), sec_header.c_str()) == 0) {
                found_sec = true;
                size_t j = i + 1;
                for (; j < lines.size(); ++j) {
                    std::string jt = lines[j];
                    jt.erase(0, jt.find_first_not_of(" \t\r\n"));
                    jt.erase(jt.find_last_not_of(" \t\r\n") + 1);
                    if (jt.rfind("[", 0) == 0) {
                        break;
                    }
                    for (const auto &kv : kvs) {
                        if (jt.rfind(kv.first + "=", 0) == 0) {
                            lines[j] = kv.first + "=" + kv.second;
                        }
                    }
                }
                for (const auto &kv : kvs) {
                    bool exists = false;
                    for (size_t k = i + 1; k < j; ++k) {
                        std::string kt = lines[k];
                        kt.erase(0, kt.find_first_not_of(" \t\r\n"));
                        if (kt.rfind(kv.first + "=", 0) == 0) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        lines.insert(lines.begin() + j, kv.first + "=" + kv.second);
                        j++;
                    }
                }
                break;
            }
        }

        if (!found_sec) {
            lines.push_back("");
            lines.push_back(sec_header);
            for (const auto &kv : kvs) {
                lines.push_back(kv.first + "=" + kv.second);
            }
        }
    }

    std::ofstream out(temp_preset, std::ios::trunc);
    for (const auto &l : lines) {
        out << l << "\n";
    }
    out.close();

    return temp_preset;
}

int CliRender::execute(const CliOptions &opts) {
    fs::path active_preset;
    fs::path temp_preset_to_delete;
    fs::path extra_shader_dir;
    std::vector<std::string> target_shaders;

    if (!opts.shader_path.empty()) {
        if (!fs::exists(opts.shader_path)) {
            std::cerr << "Error: Shader file not found: " << wide_to_utf8(opts.shader_path.c_str()) << "\n";
            return 1;
        }
        extra_shader_dir = fs::absolute(opts.shader_path).parent_path();
        target_shaders.push_back(fs::path(opts.shader_path).filename().string());

        if (!opts.preset_path.empty() && fs::exists(opts.preset_path)) {
            std::string sec = fs::path(opts.shader_path).filename().string();
            temp_preset_to_delete = create_preset_with_overrides(opts.preset_path, sec, opts.uniform_overrides);
            active_preset = temp_preset_to_delete;
        } else {
            temp_preset_to_delete = synthesize_shader_preset(opts.shader_path, opts.technique_name, opts.uniform_overrides);
            active_preset = temp_preset_to_delete;
        }
    } else if (!opts.preset_path.empty()) {
        if (!fs::exists(opts.preset_path)) {
            std::cerr << "Error: Preset file not found: " << wide_to_utf8(opts.preset_path.c_str()) << "\n";
            return 1;
        }
        if (!opts.uniform_overrides.empty()) {
            temp_preset_to_delete = create_preset_with_overrides(opts.preset_path, "", opts.uniform_overrides);
            active_preset = temp_preset_to_delete;
        } else {
            active_preset = opts.preset_path;
        }
        target_shaders = extract_shaders_from_preset(active_preset);
    } else {
        std::cerr << "Error: Neither --preset nor --shader specified for 'render'.\n";
        std::cerr << "Usage: ShaderLab.exe render (--preset <preset.ini> | --shader <shader.fx>) --input <file_or_dir> [--output <out>] [--set var=val]\n";
        return 1;
    }

    struct TempPresetGuard {
        fs::path file;
        ~TempPresetGuard() {
            if (!file.empty()) {
                std::error_code ec;
                fs::remove(file, ec);
            }
        }
    } preset_guard{temp_preset_to_delete};

    if (opts.input_path.empty()) {
        std::cerr << "Error: No input specified for 'render'.\n";
        return 1;
    }

    if (!fs::exists(opts.input_path)) {
        std::cerr << "Error: Input path not found: " << wide_to_utf8(opts.input_path.c_str()) << "\n";
        return 1;
    }

    // configure reshade preset and search paths in ini with RAII guard to restore on exit
    fs::path ini_path = find_reshade_ini_path();
    ReShadeIniGuard ini_guard(ini_path);
    configure_reshade_ini(ini_path, active_preset, extra_shader_dir);

    // clear previous log so we only inspect current invocation
    clear_reshade_log_files(extra_shader_dir.empty() ? ini_path : extra_shader_dir);

    std::vector<fs::path> images;
    if (fs::is_directory(opts.input_path)) {
        for (const auto &entry : fs::directory_iterator(opts.input_path)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)::tolower(c); });
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") {
                    images.push_back(entry.path());
                }
            }
        }
    } else {
        images.push_back(opts.input_path);
    }

    if (images.empty()) {
        std::cerr << "Error: No valid images found in: " << wide_to_utf8(opts.input_path.c_str()) << "\n";
        return 1;
    }

    fs::path out_dir;
    if (!opts.output_path.empty()) {
        out_dir = opts.output_path;
        if (images.size() > 1 || fs::is_directory(opts.output_path) || !out_dir.has_extension()) {
            fs::create_directories(out_dir);
        }
    } else {
        if (fs::is_directory(opts.input_path)) {
            out_dir = fs::path(opts.input_path) / "rendered";
        } else {
            out_dir = fs::path(opts.input_path).parent_path() / "rendered";
        }
        fs::create_directories(out_dir);
    }

    if (!opts.quiet && !opts.json_output) {
        std::cout << "========================================\n"
                  << "  ShaderLab Batch Preset Renderer\n"
                  << "========================================\n";
        if (!opts.shader_path.empty()) {
            std::cout << "Shader:        " << wide_to_utf8(opts.shader_path.c_str()) << "\n";
            if (!opts.technique_name.empty()) {
                std::cout << "Technique:     " << wide_to_utf8(opts.technique_name.c_str()) << "\n";
            }
        }
        if (!opts.preset_path.empty()) {
            std::cout << "Preset:        " << wide_to_utf8(opts.preset_path.c_str()) << "\n";
        }
        if (!opts.uniform_overrides.empty()) {
            std::cout << "Uniforms:      ";
            for (size_t i = 0; i < opts.uniform_overrides.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << opts.uniform_overrides[i].first << "=" << opts.uniform_overrides[i].second;
            }
            std::cout << "\n";
        }
        std::cout << "Input:         " << wide_to_utf8(opts.input_path.c_str()) << " (" << images.size() << " images)\n"
                  << "Output:        " << wide_to_utf8(out_dir.wstring().c_str()) << "\n"
                  << "Settle Frames: " << opts.settle_frames << "\n"
                  << "Format:        " << opts.format << "\n"
                  << "========================================\n\n";
    }

    // shared memory ipc
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
        std::cerr << "Error: Failed to create IPC shared memory mapping\n";
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
    control_block->export_settle_frames = opts.settle_frames;
    control_block->export_flags = EXPORT_FLAG_WYSIWYG;
    control_block->view_zoom = 1.0f;
    control_block->effects_ready = 0;

    // query initial dimensions to avoid early resize
    int init_w = 1280, init_h = 720;
    FILE *f_test = nullptr;
    _wfopen_s(&f_test, images[0].c_str(), L"rb");
    if (f_test) {
        int comp = 0;
        stbi_info_from_file(f_test, &init_w, &init_h, &comp);
        fclose(f_test);
    }

    GfxDevice gfx;
    if (!gfx.initialize(L"ShaderLab - Batch Renderer", 640, 360, init_w, init_h, false)) {
        std::cerr << "Error: Failed to initialize D3D11 graphics device\n";
        ini_guard.restore();
        UnmapViewOfFile(control_block);
        CloseHandle(hMap);
        return 1;
    }

    BlitRenderer blit;
    if (!blit.initialize(gfx.get_device())) {
        std::cerr << "Error: Failed to initialize BlitRenderer\n";
        gfx.shutdown();
        ini_guard.restore();
        UnmapViewOfFile(control_block);
        CloseHandle(hMap);
        return 1;
    }

    // warm up shaders directly on the first image
    LoadedImage first_img;
    bool has_first_img = ImageLoader::load_from_file(gfx.get_device(), images[0].c_str(), first_img);
    if (has_first_img) {
        control_block->depth_valid = first_img.has_depth ? 1 : 0;
        control_block->depth_far_plane = first_img.far_plane;
        control_block->depth_srv_ptr = reinterpret_cast<uint64_t>(gfx.get_depth_canvas_srv());
        control_block->depth_version++;
    }

    fs::path preset_abs = fs::absolute(active_preset);
    std::string preset_u8 = wide_to_utf8(preset_abs.wstring().c_str());

    // sync preset and wait for compilation
    wcsncpy_s(control_block->requested_preset_path, preset_abs.wstring().c_str(), _TRUNCATE);
    control_block->requested_preset_version = 1;

    if (!opts.quiet && !opts.json_output) {
        std::cout << "[ShaderLab] Compiling ReShade shader effects and loading preset..." << std::flush;
    }

    MSG msg = {};
    auto warm_start = std::chrono::steady_clock::now();
    uint32_t warm_frames = 0;
    bool compile_failed = false;
    std::string compile_error_summary;

    while (warm_frames < 60 || control_block->effects_ready == 0) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (has_first_img) {
            if (first_img.has_depth && first_img.depth_srv) {
                gfx.set_depth_render_target();
                gfx.clear_depth_canvas(1.0f);
                blit.render_depth(gfx.get_context(), first_img.depth_srv.Get(), first_img.far_plane);
            }
            gfx.set_render_target();
            const float clear_c[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            gfx.clear(clear_c);
            blit.render(gfx.get_context(), first_img.srv.Get());
        } else {
            const float c[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            gfx.clear(c);
        }

        gfx.present(1, 0);
        warm_frames++;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));

        // monitor ReShade.log during warm-up to fail immediately on compile errors
        std::string log_content = read_reshade_log(extra_shader_dir.empty() ? ini_path : extra_shader_dir);
        if (!log_content.empty()) {
            if (!target_shaders.empty()) {
                for (const auto &sh : target_shaders) {
                    bool failed = false, succeeded = false;
                    std::string err_text;
                    if (check_shader_compile_in_log(log_content, sh, failed, succeeded, err_text)) {
                        if (failed) {
                            compile_failed = true;
                            compile_error_summary = err_text;
                            break;
                        }
                    }
                }
            } else if (log_content.find("Failed to compile") != std::string::npos) {
                compile_failed = true;
                auto errors = parse_reshade_log_errors(log_content);
                std::stringstream ss;
                for (const auto &e : errors) {
                    if (!e.is_warning) {
                        ss << "  " << e.file << "(" << e.line << "," << e.column << "): [ERROR] "
                           << e.code << ": " << e.message << "\n";
                    }
                }
                compile_error_summary = ss.str();
            }
        }

        if (compile_failed) {
            break;
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - warm_start).count() >= 10) {
            break;
        }
    }

    if (compile_failed || control_block->effects_ready == 0) {
        if (!opts.quiet && !opts.json_output) {
            std::cerr << "\n[ShaderLab] Error: Shader compilation failed or timed out.\n";
            if (!compile_error_summary.empty()) {
                std::cerr << compile_error_summary << "\n";
            }
        }
        gfx.shutdown();
        ini_guard.restore();
        UnmapViewOfFile(control_block);
        CloseHandle(hMap);
        return 1;
    }

    if (!opts.quiet && !opts.json_output) {
        std::cout << " Ready!\n\n";
    }

    int success_count = 0;
    auto t_start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < images.size(); ++i) {
        const auto &img_p = images[i];
        std::string fname = img_p.filename().string();

        if (!opts.quiet && !opts.json_output) {
            std::cout << "[" << (i + 1) << "/" << images.size() << "] Rendering: " << fname << "... " << std::flush;
        }

        LoadedImage img;
        if (i == 0 && has_first_img) {
            img = std::move(first_img);
        } else if (!ImageLoader::load_from_file(gfx.get_device(), img_p.c_str(), img)) {
            if (!opts.quiet && !opts.json_output) std::cout << "FAILED (Image load error)\n";
            continue;
        }

        control_block->requested_preset_version++;

        // sync depth metadata so reshade gets proper texture pointers
        control_block->depth_valid = img.has_depth ? 1 : 0;
        control_block->depth_far_plane = img.far_plane;
        control_block->depth_width = img.width;
        control_block->depth_height = img.height;
        control_block->depth_srv_ptr = reinterpret_cast<uint64_t>(gfx.get_depth_canvas_srv());
        control_block->depth_version++;

        if (gfx.get_render_width() != img.width || gfx.get_render_height() != img.height) {
            control_block->effects_compiling = 1;
            control_block->effects_ready = 0;
            gfx.resize_buffers(img.width, img.height);

            // update depth srv pointer immediately after resize so reshade doesnt sample freed memory
            control_block->depth_srv_ptr = reinterpret_cast<uint64_t>(gfx.get_depth_canvas_srv());
            control_block->depth_width = gfx.get_render_width();
            control_block->depth_height = gfx.get_render_height();
            control_block->depth_version++;

            // wait for effects to compile for new size
            auto recompile_start = std::chrono::steady_clock::now();
            while (control_block->effects_ready == 0 || control_block->effects_compiling != 0) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                if (img.has_depth && img.depth_srv) {
                    gfx.set_depth_render_target();
                    gfx.clear_depth_canvas(1.0f);
                    blit.render_depth(gfx.get_context(), img.depth_srv.Get(), img.far_plane);
                }
                gfx.set_render_target();
                const float clear_c[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                gfx.clear(clear_c);
                blit.render(gfx.get_context(), img.srv.Get());

                gfx.present(1, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));

                std::string re_log = read_reshade_log(extra_shader_dir.empty() ? ini_path : extra_shader_dir);
                if (!target_shaders.empty()) {
                    bool re_failed = false, re_succ = false;
                    std::string re_err;
                    for (const auto &sh : target_shaders) {
                        if (check_shader_compile_in_log(re_log, sh, re_failed, re_succ, re_err) && re_failed) {
                            break;
                        }
                    }
                    if (re_failed) break;
                }

                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - recompile_start).count() >= 15) {
                    break;
                }
            }
        }

        fs::path dest_file;
        fs::path out_p_path = opts.output_path;
        if (images.size() == 1 && out_p_path.has_extension() && !fs::is_directory(out_p_path)) {
            dest_file = out_p_path;
        } else {
            std::string out_ext = (opts.format == "jpg" || opts.format == "jpeg") ? ".jpg" : ".png";
            fs::path stem = img_p.stem();
            dest_file = out_dir / (stem.string() + out_ext);
        }

        wcsncpy_s(control_block->export_input_path, img_p.c_str(), _TRUNCATE);
        wcsncpy_s(control_block->export_output_path, dest_file.c_str(), _TRUNCATE);
        control_block->export_state = ExportState::Rendering;
        control_block->export_flags = EXPORT_FLAG_AUTO_CONVERGE;
        control_block->export_settle_frames = opts.settle_frames;
        control_block->export_frame_index = 0;

        for (int frame = 0; frame < opts.settle_frames; ++frame) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            if (img.has_depth && img.depth_srv) {
                gfx.set_depth_render_target();
                gfx.clear_depth_canvas(1.0f);
                blit.render_depth(gfx.get_context(), img.depth_srv.Get(), img.far_plane);
            }

            gfx.set_render_target();
            const float clear_c[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            gfx.clear(clear_c);
            blit.render(gfx.get_context(), img.srv.Get());

            gfx.present(1, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        control_block->export_state = ExportState::Capturing;

        auto cap_start = std::chrono::steady_clock::now();
        bool captured_by_addon = false;

        while (true) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            if (control_block->export_state == ExportState::Done) {
                captured_by_addon = true;
                break;
            }
            if (control_block->export_state == ExportState::Failed) {
                break;
            }

            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cap_start).count();
            if (elapsed_ms >= 180000) { // generous 3 minute timeout for huge 16k/32k image compression
                break;
            }

            if (img.has_depth && img.depth_srv) {
                gfx.set_depth_render_target();
                gfx.clear_depth_canvas(1.0f);
                blit.render_depth(gfx.get_context(), img.depth_srv.Get(), img.far_plane);
            }
            gfx.set_render_target();
            const float clear_c[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            gfx.clear(clear_c);
            blit.render(gfx.get_context(), img.srv.Get());

            gfx.present(1, 0);

            if (control_block->export_state == ExportState::Done) {
                captured_by_addon = true;
                break;
            }
            if (control_block->export_state == ExportState::Failed) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        // strictly require addon capture: do NOT save raw unshaded backbuffer on capture failure
        bool ok = false;
        if (captured_by_addon && fs::exists(dest_file)) {
            ok = true;
        } else {
            ok = false;
        }

        control_block->export_state = ExportState::Idle;

        if (ok) {
            ++success_count;
            if (!opts.quiet && !opts.json_output) std::cout << "DONE -> " << dest_file.filename().string() << "\n";
        } else {
            if (!opts.quiet && !opts.json_output) std::cout << "FAILED (Shader effect capture failed)\n";
        }
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double elapsed_sec = std::chrono::duration<double>(t_end - t_start).count();

    gfx.shutdown();
    ini_guard.restore();
    UnmapViewOfFile(control_block);
    CloseHandle(hMap);

    if (opts.json_output) {
        std::cout << "{\n";
        if (!opts.shader_path.empty()) {
            std::cout << "  \"shader\": \"" << json_escape(wide_to_utf8(opts.shader_path.c_str())) << "\",\n";
        }
        std::cout << "  \"preset\": \"" << json_escape(preset_u8) << "\",\n"
                  << "  \"total_images\": " << images.size() << ",\n"
                  << "  \"rendered_success\": " << success_count << ",\n"
                  << "  \"rendered_failed\": " << (images.size() - success_count) << ",\n"
                  << "  \"elapsed_seconds\": " << elapsed_sec << ",\n"
                  << "  \"average_render_ms\": " << (images.size() > 0 ? (elapsed_sec * 1000.0 / images.size()) : 0.0) << "\n"
                  << "}\n";
    } else if (!opts.quiet) {
        std::cout << "\n========================================\n"
                  << "Render Complete: " << success_count << "/" << images.size() << " images rendered successfully in "
                  << std::fixed << std::setprecision(2) << elapsed_sec << "s\n"
                  << "Output Location: " << wide_to_utf8(out_dir.wstring().c_str()) << "\n"
                  << "========================================\n";
    }

    return (success_count == static_cast<int>(images.size())) ? 0 : 1;
}
