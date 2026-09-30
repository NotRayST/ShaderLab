#include "cli_parser.h"
#include <windows.h>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

#include "../../../common/str_utils.h"

using str_utils::wide_to_utf8;
using str_utils::utf8_to_wide;

static bool is_cmd_match(const wchar_t *arg, const wchar_t *target) {
    return _wcsicmp(arg, target) == 0;
}

static std::wstring clean_quotes(std::wstring str) {
    if (str.size() >= 2 && str.front() == L'"' && str.back() == L'"') {
        str = str.substr(1, str.size() - 2);
    }
    return str;
}

static std::wstring consume_path_arg(int &idx, int argc, wchar_t **argv) {
    if (idx + 1 >= argc) return {};
    std::wstring result = clean_quotes(argv[++idx]);

    while (idx + 1 < argc && argv[idx + 1][0] != L'-') {
        result += L" " + clean_quotes(argv[++idx]);
    }
    return result;
}

bool CliParser::is_cli_invocation(int argc, wchar_t **argv) {
    if (argc <= 1) return false;

    const wchar_t *first = argv[1];
    if (is_cmd_match(first, L"render") ||
        is_cmd_match(first, L"test-shader") ||
        is_cmd_match(first, L"depth") ||
        is_cmd_match(first, L"info") ||
        is_cmd_match(first, L"preset") ||
        is_cmd_match(first, L"ctl") ||
        is_cmd_match(first, L"control") ||
        is_cmd_match(first, L"selftest") ||
        is_cmd_match(first, L"--selftest") ||
        is_cmd_match(first, L"help") ||
        is_cmd_match(first, L"--help") ||
        is_cmd_match(first, L"-h") ||
        is_cmd_match(first, L"--version") ||
        is_cmd_match(first, L"-v")) {
        return true;
    }

    for (int i = 1; i < argc; ++i) {
        if (is_cmd_match(argv[i], L"--preset") ||
            is_cmd_match(argv[i], L"--shader") ||
            is_cmd_match(argv[i], L"--json") ||
            is_cmd_match(argv[i], L"--selftest")) {
            return true;
        }
    }

    return false;
}

CliOptions CliParser::parse(int argc, wchar_t **argv) {
    CliOptions opts;
    if (argc <= 1) return opts;

    int idx = 1;
    const wchar_t *first = argv[1];

    if (is_cmd_match(first, L"render")) {
        opts.subcommand = "render";
        idx = 2;
    } else if (is_cmd_match(first, L"test-shader") || is_cmd_match(first, L"testshader")) {
        opts.subcommand = "test-shader";
        idx = 2;
    } else if (is_cmd_match(first, L"depth")) {
        opts.subcommand = "depth";
        idx = 2;
    } else if (is_cmd_match(first, L"info")) {
        opts.subcommand = "info";
        idx = 2;
    } else if (is_cmd_match(first, L"preset")) {
        opts.subcommand = "preset";
        idx = 2;
    } else if (is_cmd_match(first, L"ctl") || is_cmd_match(first, L"control")) {
        opts.subcommand = "ctl";
        idx = 2;
    } else if (is_cmd_match(first, L"selftest") || is_cmd_match(first, L"--selftest")) {
        opts.subcommand = "selftest";
        idx = 2;
    } else if (is_cmd_match(first, L"help") || is_cmd_match(first, L"--help") || is_cmd_match(first, L"-h")) {
        opts.subcommand = "help";
        opts.help = true;
        idx = 2;
    } else if (is_cmd_match(first, L"--version") || is_cmd_match(first, L"-v")) {
        opts.subcommand = "version";
        opts.version = true;
        idx = 2;
    }

    while (idx < argc) {
        const wchar_t *arg = argv[idx];

        if (is_cmd_match(arg, L"--input") || is_cmd_match(arg, L"-i") || is_cmd_match(arg, L"--image")) {
            opts.input_path = consume_path_arg(idx, argc, argv);
        } else if (is_cmd_match(arg, L"--output") || is_cmd_match(arg, L"-o")) {
            opts.output_path = consume_path_arg(idx, argc, argv);
        } else if (is_cmd_match(arg, L"--preset") || is_cmd_match(arg, L"-p")) {
            opts.preset_path = consume_path_arg(idx, argc, argv);
        } else if (is_cmd_match(arg, L"--preset-b") || is_cmd_match(arg, L"--diff")) {
            opts.preset_path_b = consume_path_arg(idx, argc, argv);
        } else if (is_cmd_match(arg, L"--shader") || is_cmd_match(arg, L"-s")) {
            opts.shader_path = consume_path_arg(idx, argc, argv);
        } else if (is_cmd_match(arg, L"--technique") || is_cmd_match(arg, L"-t")) {
            opts.technique_name = consume_path_arg(idx, argc, argv);
        } else if (is_cmd_match(arg, L"--set") || is_cmd_match(arg, L"--uniform") || is_cmd_match(arg, L"-u")) {
            if (idx + 1 < argc) {
                std::wstring param_str = clean_quotes(argv[++idx]);
                size_t eq_pos = param_str.find(L'=');
                if (eq_pos != std::wstring::npos) {
                    std::string key = wide_to_utf8(param_str.substr(0, eq_pos).c_str());
                    std::string val = wide_to_utf8(param_str.substr(eq_pos + 1).c_str());
                    opts.uniform_overrides.push_back({ key, val });
                } else if (idx + 1 < argc && argv[idx + 1][0] != L'-') {
                    std::string key = wide_to_utf8(param_str.c_str());
                    std::string val = wide_to_utf8(clean_quotes(argv[++idx]).c_str());
                    opts.uniform_overrides.push_back({ key, val });
                }
            }
        } else if (is_cmd_match(arg, L"--encoder") || is_cmd_match(arg, L"-m")) {
            if (idx + 1 < argc) opts.encoder = wide_to_utf8(argv[++idx]);
        } else if (is_cmd_match(arg, L"--settle-frames") || is_cmd_match(arg, L"-f")) {
            if (idx + 1 < argc) opts.settle_frames = _wtoi(argv[++idx]);
        } else if (is_cmd_match(arg, L"--far-plane")) {
            if (idx + 1 < argc) opts.far_plane = static_cast<float>(_wtof(argv[++idx]));
        } else if (is_cmd_match(arg, L"--format")) {
            if (idx + 1 < argc) opts.format = wide_to_utf8(argv[++idx]);
        } else if (is_cmd_match(arg, L"--embed-png") || is_cmd_match(arg, L"--embed")) {
            opts.embed_png = true;
        } else if (is_cmd_match(arg, L"--sidecar") || is_cmd_match(arg, L"--depth-sidecar")) {
            opts.sidecar = true;
        } else if (is_cmd_match(arg, L"--json")) {
            opts.json_output = true;
        } else if (is_cmd_match(arg, L"--quiet") || is_cmd_match(arg, L"-q")) {
            opts.quiet = true;
        } else if (is_cmd_match(arg, L"--help") || is_cmd_match(arg, L"-h")) {
            opts.help = true;
        } else if (is_cmd_match(arg, L"--version") || is_cmd_match(arg, L"-v")) {
            opts.version = true;
        } else {
            // positional arg fallback
            if (opts.subcommand == "ctl" || opts.subcommand == "control") {
                opts.extra_args.push_back(clean_quotes(argv[idx]));
            } else {
                --idx; // unconsume arg so consume_path_arg can read it and any continuation tokens
                std::wstring pos_arg = consume_path_arg(idx, argc, argv);
                if (opts.subcommand == "render" || opts.subcommand == "depth" || opts.subcommand == "info") {
                    if (opts.input_path.empty()) {
                        opts.input_path = pos_arg;
                    } else if (opts.output_path.empty()) {
                        opts.output_path = pos_arg;
                    } else {
                        opts.extra_args.push_back(pos_arg);
                    }
                } else if (opts.subcommand == "test-shader") {
                    if (opts.shader_path.empty()) {
                        opts.shader_path = pos_arg;
                    } else {
                        opts.extra_args.push_back(pos_arg);
                    }
                } else if (opts.subcommand == "preset") {
                    if (opts.preset_path.empty()) {
                        opts.preset_path = pos_arg;
                    } else if (opts.preset_path_b.empty()) {
                        opts.preset_path_b = pos_arg;
                    } else {
                        opts.extra_args.push_back(pos_arg);
                    }
                } else {
                    opts.extra_args.push_back(pos_arg);
                }
            }
        }
        ++idx;
    }

    return opts;
}

void CliParser::print_version() {
    std::cout << "ShaderLab CLI v1.2.2 (Direct3D 11 / ReShade FX Pipeline)\n";
    std::cout << "Copyright (c) 2026 NotRayST. All rights reserved.\n";
}

void CliParser::print_help(const std::string &subcommand) {
    if (subcommand == "render") {
        std::cout << "Usage: ShaderLab.exe render [options]\n\n"
                  << "Headlessly renders an image or directory of images through a ReShade preset or direct .fx shader.\n"
                  << "Supports overriding shader uniform parameters on the fly for rapid headless testing.\n\n"
                  << "Options:\n"
                  << "  -s, --shader <path>        Directly render a .fx shader file (no .ini preset required)\n"
                  << "  -t, --technique <name>     Specify technique name to run when using --shader\n"
                  << "  -u, --set <var>=<val>      Override shader uniform parameter (can be repeated)\n"
                  << "  -p, --preset <path>        Path to ReShade .ini preset file\n"
                  << "  -i, --input, --image <p>   Path to input image or directory (Required)\n"
                  << "  -o, --output <path>        Path to output file or directory (Default: input_dir/rendered/)\n"
                  << "  -f, --settle-frames <N>    Number of convergence frames (Default: 45)\n"
                  << "      --format <png|jpg>     Output image format (Default: png)\n"
                  << "      --quiet                Suppress non-essential console output\n"
                  << "      --json                 Emit machine-readable JSON progress\n"
                  << "  -h, --help                 Show this help message\n\n"
                  << "Examples:\n"
                  << "  ShaderLab.exe render --shader DisplayDepth.fx --input test.png --output out.png --set iUIFar=1000.0\n"
                  << "  ShaderLab.exe render --preset presets/Cinematic.ini --input C:/Screenshots/ --output C:/Rendered/\n";
        return;
    }

    if (subcommand == "test-shader") {
        std::cout << "Usage: ShaderLab.exe test-shader [options]\n\n"
                  << "Headlessly compiles a ReShade .fx shader and reports syntax/compilation errors.\n"
                  << "Optionally renders the shader onto an image to produce a visual picture headlessly.\n\n"
                  << "Options:\n"
                  << "  -s, --shader <path>        Path to .fx or .fxh shader file (Required)\n"
                  << "  -i, --image, --input <p>   Optional input image to render shader onto headlessly\n"
                  << "  -o, --output <path>        Optional output image path when --image is specified\n"
                  << "  -t, --technique <name>     Specify technique name to activate\n"
                  << "  -u, --set <var>=<val>      Override shader uniform parameter (can be repeated)\n"
                  << "  -p, --preset <path>        Optional preset to test with\n"
                  << "      --json                 Output structured JSON error report (Ideal for AI agents)\n"
                  << "      --quiet                Quiet mode (Exit code 0 on success, 1 on error)\n"
                  << "  -h, --help                 Show this help message\n\n"
                  << "Examples:\n"
                  << "  ShaderLab.exe test-shader --shader shaders/MyEffect.fx --json\n"
                  << "  ShaderLab.exe test-shader --shader shaders/DisplayDepth.fx --image sample.png --output depth.png\n";
        return;
    }

    if (subcommand == "depth") {
        std::cout << "Usage: ShaderLab.exe depth [options]\n\n"
                  << "Batch generates AI 3D depth maps (Depth Anything V2) for images.\n\n"
                  << "Options:\n"
                  << "  -i, --input <path>         Path to image or directory (Required)\n"
                  << "  -m, --encoder <size>       Model size: vits, vitb, vitl (Default: vitl)\n"
                  << "      --far-plane <val>      Virtual far plane depth (Default: 1000.0)\n"
                  << "      --embed-png            Embed slDp chunk inside PNGs instead of .sldepth\n"
                  << "  -h, --help                 Show this help message\n\n"
                  << "Example:\n"
                  << "  ShaderLab.exe depth --input C:/Screenshots/ --encoder vitl --embed-png\n";
        return;
    }

    if (subcommand == "info") {
        std::cout << "Usage: ShaderLab.exe info [options] <image_path>\n\n"
                  << "Inspects image metadata, embedded 3D depth chunks (slDp), and sidecars (.sldepth).\n\n"
                  << "Options:\n"
                  << "      --json                 Emit machine-readable JSON metadata\n"
                  << "  -h, --help                 Show this help message\n\n"
                  << "Example:\n"
                  << "  ShaderLab.exe info screenshot.png --json\n";
        return;
    }

    if (subcommand == "preset") {
        std::cout << "Usage: ShaderLab.exe preset [options] <preset_a> [<preset_b>]\n\n"
                  << "Inspects preset active techniques or diffs two presets.\n\n"
                  << "Options:\n"
                  << "  -d, --diff <preset_b>      Compare preset_a against preset_b\n"
                  << "      --json                 Emit machine-readable JSON structure\n"
                  << "  -h, --help                 Show this help message\n\n"
                  << "Example:\n"
                  << "  ShaderLab.exe preset PresetA.ini --diff PresetB.ini\n";
        return;
    }

    if (subcommand == "ctl" || subcommand == "control") {
        std::cout << "Usage: ShaderLab.exe ctl <subcommand> [options]\n\n"
                  << "Remote control and query running ShaderLab instance via IPC mailbox.\n\n"
                  << "Subcommands:\n"
                  << "  status                         Get telemetry, viewport, depth, and export status\n"
                  << "  snapshot <out_path>            Capture live viewport backbuffer to PNG/JPG\n"
                  << "  view [zoom] [angle] [x] [y]    Set viewport zoom, rotation angle, and pan offsets\n"
                  << "  action <name>                  Trigger UI action (reset, undo, redo, nudge, lock, etc.)\n"
                  << "  load <image_path>              Load image into host preview\n"
                  << "  export [out_path]              Trigger full ReShade export sequence\n"
                  << "  depth-peek <0|1>               Toggle depth map peek mode\n"
                  << "  resize <w> <h>                 Resize host window and swapchain\n"
                  << "  reshade-toggle [0|1]           Toggle master ReShade effect processing\n"
                  << "  reshade-reload                 Reload all ReShade shaders\n"
                  << "  reshade-overlay [0|1]          Open or close ReShade ImGui overlay\n"
                  << "  reshade-technique <name> [0|1] Toggle specific ReShade technique\n"
                  << "  reshade-uniform <name> <v0..>  Tweak ReShade uniform variable value(s)\n"
                  << "  reshade-preset <path>          Switch active ReShade preset\n\n"
                  << "Options:\n"
                  << "      --json                     Emit machine-readable JSON output\n"
                  << "  -h, --help                     Show this help message\n\n"
                  << "Example:\n"
                  << "  ShaderLab.exe ctl status --json\n"
                  << "  ShaderLab.exe ctl snapshot scratch/test.png\n";
        return;
    }

    // default usage text
    print_version();
    std::cout << "\nUsage: ShaderLab.exe <command> [options]\n\n"
              << "Available Commands:\n"
              << "  render         Apply ReShade presets to images or folders (Batch processing)\n"
              << "  test-shader    Test and validate ReShade .fx shader compilation (AI/Dev tool)\n"
              << "  depth          Batch generate 3D depth maps with Depth Anything V2\n"
              << "  info           Inspect image depth metadata, PNG chunks, and sidecars\n"
              << "  preset         Inspect or diff ReShade preset files\n"
              << "  ctl            Remote control running ShaderLab instance (CLI/MCP automation)\n"
              << "  selftest       Run hardware GPU blit & backbuffer capture diagnostics\n"
              << "  help           Display help for a specific command\n\n"
              << "Run 'ShaderLab.exe <command> --help' for details on a specific command.\n"
              << "Run 'ShaderLab.exe' without arguments to launch the interactive GUI.\n";
}
