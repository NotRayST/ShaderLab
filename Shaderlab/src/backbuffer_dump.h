#pragma once
#include <windows.h>
#include <string>
#include "gfx_device.h"
#include "blit_renderer.h"

class BackbufferDump {
public:
    static bool run_selftest(GfxDevice &gfx, BlitRenderer &blit, const wchar_t *input_path, const wchar_t *output_path = nullptr);
    static bool capture_to_file(GfxDevice &gfx, const wchar_t *output_path, uint32_t width, uint32_t height);
};
