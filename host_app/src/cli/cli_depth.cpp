#include "cli_depth.h"
#include "../../common/depth_chunk.h"
#include "../../common/depth_file.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <chrono>
#include <windows.h>
#include <algorithm>

#include "../../third_party/stb/stb_image.h"

#include "../../common/str_utils.h"

namespace fs = std::filesystem;
using str_utils::wide_to_utf8;
using str_utils::utf8_to_wide;

static std::wstring find_python() {
    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        fs::path base = fs::path(exe_path).parent_path();
        for (int i = 0; i <= 3; ++i) {
            std::vector<fs::path> local_candidates = {
                base / "tools" / "python" / "python.exe",
                base / "python" / "python.exe",
                base / "tools" / "depth_anything_v2" / "venv" / "Scripts" / "python.exe",
                base / "tools" / "depth_anything_v2" / "python" / "python.exe",
                base / "venv" / "Scripts" / "python.exe",
                base / ".venv" / "Scripts" / "python.exe"
            };
            for (const auto &p : local_candidates) {
                if (fs::exists(p)) return fs::absolute(p).wstring();
            }
            base = base.parent_path();
        }
    }

    wchar_t user_prof[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", user_prof, MAX_PATH) > 0) {
        std::wstring conda = std::wstring(user_prof) + L"\\miniconda3\\python.exe";
        if (fs::exists(conda)) return conda;
        std::wstring ana = std::wstring(user_prof) + L"\\anaconda3\\python.exe";
        if (fs::exists(ana)) return ana;
    }
    std::vector<std::wstring> known = {
        L"C:\\Python314\\python.exe",
        L"C:\\Python313\\python.exe",
        L"C:\\Python312\\python.exe",
        L"C:\\Python311\\python.exe",
        L"C:\\Python310\\python.exe",
        L"C:\\Program Files\\Python312\\python.exe",
        L"C:\\Program Files\\Python311\\python.exe"
    };
    for (const auto &p : known) {
        if (fs::exists(p)) return p;
    }
    return L"python.exe";
}

static std::wstring find_script() {
    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        fs::path base = fs::path(exe_path).parent_path();
        for (int i = 0; i <= 6; ++i) {
            fs::path p = base / "tools" / "depth_anything_v2" / "run_depth_anything.py";
            if (fs::exists(p)) return fs::absolute(p).wstring();
            base = base.parent_path();
        }
    }
    return L"tools/depth_anything_v2/run_depth_anything.py";
}

static std::wstring find_model(const std::string &encoder) {
    std::wstring enc_w = utf8_to_wide(encoder.c_str());
    std::vector<std::wstring> names = {
        L"depth_anything_v2_" + enc_w + L".pth",
        L"depth_anything_v2" + enc_w + L".pth",
        L"depth anything V2 " + enc_w + L".pth"
    };
    if (enc_w == L"vits") {
        names.push_back(L"depth_anything_v2.pth");
        names.push_back(L"depth anything V2.pth");
    }

    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        fs::path base = fs::path(exe_path).parent_path();
        for (int i = 0; i <= 6; ++i) {
            for (const auto &n : names) {
                if (fs::exists(base / n)) return fs::absolute(base / n).wstring();
                if (fs::exists(base / "common" / n)) return fs::absolute(base / "common" / n).wstring();
            }
            base = base.parent_path();
        }
    }
    return L"common/depth_anything_v2_" + enc_w + L".pth";
}

int CliDepth::execute(const CliOptions &opts) {
    if (opts.input_path.empty()) {
        std::cerr << "Error: No input specified for 'depth' command.\n";
        std::cerr << "Usage: ShaderLab.exe depth --input <file_or_dir> [--encoder vitl] [--embed-png]\n";
        return 1;
    }

    if (!fs::exists(opts.input_path)) {
        std::cerr << "Error: Input path does not exist: " << wide_to_utf8(opts.input_path.c_str()) << "\n";
        return 1;
    }

    std::vector<fs::path> images;
    if (fs::is_directory(opts.input_path)) {
        for (const auto &entry : fs::recursive_directory_iterator(opts.input_path)) {
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
        std::cerr << "Error: No supported images (.png, .jpg, .bmp) found in: " << wide_to_utf8(opts.input_path.c_str()) << "\n";
        return 1;
    }

    std::wstring py_exe = find_python();
    std::wstring script = find_script();
    std::wstring model = find_model(opts.encoder);

    if (!fs::exists(script)) {
        std::cerr << "Error: Inference script not found: " << wide_to_utf8(script.c_str()) << "\n";
        return 1;
    }
    if (!fs::exists(model)) {
        std::cerr << "Error: Model weights not found for encoder '" << opts.encoder << "': " << wide_to_utf8(model.c_str()) << "\n";
        return 1;
    }

    if (!opts.quiet && !opts.json_output) {
        std::cout << "========================================\n"
                  << "  ShaderLab Batch AI Depth Generation\n"
                  << "========================================\n"
                  << "Files Found:  " << images.size() << "\n"
                  << "Model Size:   " << opts.encoder << "\n"
                  << "Weights:      " << wide_to_utf8(model.c_str()) << "\n"
                  << "Embed PNG:    " << (opts.embed_png ? "YES" : "NO (.sldepth companion)") << "\n"
                  << "Far Plane:    " << opts.far_plane << "\n"
                  << "========================================\n\n";
    }

    int success_count = 0;
    auto t_start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < images.size(); ++i) {
        const auto &img_p = images[i];
        std::string img_u8 = wide_to_utf8(img_p.wstring().c_str());

        if (!opts.quiet && !opts.json_output) {
            std::cout << "[" << (i + 1) << "/" << images.size() << "] Processing: " << img_p.filename().string() << "... " << std::flush;
        }

        fs::path temp_raw = img_p;
        temp_raw.replace_extension(".slraw_depth.tmp");

        fs::path target_sidecar = img_p;
        target_sidecar.replace_extension(".sldepth");

        std::wstring cmd = L"\"" + py_exe + L"\" \"" + script + L"\""
                         + L" --input \"" + img_p.wstring() + L"\""
                         + L" --model \"" + model + L"\""
                         + L" --encoder " + utf8_to_wide(opts.encoder.c_str())
                         + L" --far-plane " + std::to_wstring(opts.far_plane);

        if (opts.embed_png && img_p.extension() == ".png") {
            cmd += L" --output-raw \"" + temp_raw.wstring() + L"\"";
        } else {
            cmd += L" --output-sidecar \"" + target_sidecar.wstring() + L"\"";
        }

        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
        cmd_buf.push_back(L'\0');

        bool ok = false;
        if (CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 180000); // 3 min timeout
            DWORD exit_code = 1;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            ok = (exit_code == 0);
        }

        if (ok && opts.embed_png && img_p.extension() == ".png") {
            // read temp raw floats and inject png chunk
            std::ifstream in_raw(temp_raw, std::ios::binary | std::ios::ate);
            if (in_raw.is_open()) {
                size_t raw_size = in_raw.tellg();
                in_raw.seekg(0, std::ios::beg);
                std::vector<float> floats(raw_size / sizeof(float));
                in_raw.read(reinterpret_cast<char*>(floats.data()), raw_size);
                in_raw.close();
                fs::remove(temp_raw);

                int w = 0, h = 0, comp = 0;
                FILE *f = nullptr;
                _wfopen_s(&f, img_p.c_str(), L"rb");
                if (f) {
                    stbi_info_from_file(f, &w, &h, &comp);
                    fclose(f);
                }

                DepthMapHeader hdr = {};
                hdr.magic = kDepthMagicSLD1;
                hdr.version = kDepthVersion1;
                hdr.encoding = 0;
                hdr.width = w;
                hdr.height = h;
                hdr.flags = kDepthFlagValid;
                hdr.near_plane = 1.0f;
                hdr.far_plane = opts.far_plane;
                hdr.raw_byte_size = (uint32_t)(floats.size() * sizeof(float));
                hdr.timestamp = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                strcpy_s(hdr.game_name, "ShaderLabDepth");

                std::string err;
                ok = depth_chunk::inject_sldp(img_u8, hdr, floats.data(), floats.size(), err);
            }
        }

        if (ok) {
            ++success_count;
            if (!opts.quiet && !opts.json_output) std::cout << "DONE\n";
        } else {
            if (!opts.quiet && !opts.json_output) std::cout << "FAILED\n";
        }
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double elapsed_sec = std::chrono::duration<double>(t_end - t_start).count();

    if (opts.json_output) {
        std::cout << "{\n"
                  << "  \"total\": " << images.size() << ",\n"
                  << "  \"success\": " << success_count << ",\n"
                  << "  \"failed\": " << (images.size() - success_count) << ",\n"
                  << "  \"elapsed_seconds\": " << elapsed_sec << ",\n"
                  << "  \"encoder\": \"" << opts.encoder << "\"\n"
                  << "}\n";
    } else if (!opts.quiet) {
        std::cout << "\nBatch generation finished: " << success_count << "/" << images.size()
                  << " completed in " << std::fixed << std::setprecision(1) << elapsed_sec << "s\n";
    }

    return (success_count == (int)images.size()) ? 0 : 1;
}
