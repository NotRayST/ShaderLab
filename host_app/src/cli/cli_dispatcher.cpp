#include "cli_dispatcher.h"
#include "cli_parser.h"
#include "cli_render.h"
#include "cli_test_shader.h"
#include "cli_depth.h"
#include "cli_info.h"
#include "cli_preset.h"
#include "cli_ctl.h"
#include "../backbuffer_dump.h"
#include "../gfx_device.h"
#include "../blit_renderer.h"
#include "../../../common/str_utils.h"
#include <iostream>

using str_utils::wide_to_utf8;

bool CliDispatcher::should_run_cli(int argc, wchar_t **argv) {
    return CliParser::is_cli_invocation(argc, argv);
}

int CliDispatcher::dispatch(int argc, wchar_t **argv) {
    CliOptions opts = CliParser::parse(argc, argv);

    if (opts.help || opts.subcommand == "help") {
        CliParser::print_help(opts.subcommand == "help" && !opts.extra_args.empty() ?
            wide_to_utf8(opts.extra_args[0].c_str()) : opts.subcommand);
        return 0;
    }

    if (opts.version || opts.subcommand == "version") {
        CliParser::print_version();
        return 0;
    }

    if (opts.subcommand == "render") {
        return CliRender::execute(opts);
    }

    if (opts.subcommand == "test-shader") {
        return CliTestShader::execute(opts);
    }

    if (opts.subcommand == "depth") {
        return CliDepth::execute(opts);
    }

    if (opts.subcommand == "info") {
        return CliInfo::execute(opts);
    }

    if (opts.subcommand == "preset") {
        return CliPreset::execute(opts);
    }

    if (opts.subcommand == "ctl" || opts.subcommand == "control") {
        return CliCtl::execute(opts);
    }

    if (opts.subcommand == "selftest") {
        const wchar_t *input_path = !opts.input_path.empty() ? opts.input_path.c_str() : nullptr;
        const wchar_t *output_path = !opts.output_path.empty() ? opts.output_path.c_str() : L"_selftest.png";

        std::cout << "========================================\n"
                  << "  ShaderLab - GPU Self-Test Mode\n"
                  << "========================================\n";

        GfxDevice gfx;
        if (!gfx.initialize(L"ShaderLab - GPU Self-Test", 960, 540, 960, 540, false)) {
            std::cerr << "[SelfTest] GfxDevice initialization failed!\n";
            return 1;
        }

        BlitRenderer blit;
        if (!blit.initialize(gfx.get_device())) {
            std::cerr << "[SelfTest] BlitRenderer initialization failed!\n";
            gfx.shutdown();
            return 1;
        }

        bool ok = BackbufferDump::run_selftest(gfx, blit, input_path, output_path);
        gfx.shutdown();
        return ok ? 0 : 1;
    }

    CliParser::print_help();
    return 1;
}
