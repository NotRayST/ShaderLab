#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "capture_manager.h"

extern "C" __declspec(dllexport) const char* NAME = "ShaderLab Capture";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Captures in-game depth for ShaderLab.";

static void on_begin_effects(reshade::api::effect_runtime* runtime, reshade::api::command_list* cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb) {
    CaptureManager::get().on_reshade_begin_effects(runtime, cmd_list, rtv, rtv_srgb);
}

static void on_finish_effects(reshade::api::effect_runtime* runtime, reshade::api::command_list* cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb) {
    CaptureManager::get().on_reshade_finish_effects(runtime, cmd_list, rtv, rtv_srgb);
}

static void on_screenshot(reshade::api::effect_runtime* runtime, const char* path) {
    CaptureManager::get().on_reshade_screenshot(runtime, path);
}

static void on_reloaded_effects(reshade::api::effect_runtime* runtime) {
    CaptureManager::get().on_reshade_reloaded_effects(runtime);
}

static void on_overlay(reshade::api::effect_runtime* runtime) {
    CaptureManager::get().on_draw_overlay(runtime);
}

static void on_reshade_overlay_frame(reshade::api::effect_runtime* runtime) {
    CaptureManager::get().on_reshade_overlay_frame(runtime);
}

static bool s_standalone_tab = true;

void CaptureManager::set_standalone_tab(reshade::api::effect_runtime* runtime, bool standalone) {
    if (s_standalone_tab == standalone) return;
    reshade::unregister_overlay(s_standalone_tab ? "ShaderLab Capture" : nullptr, &on_overlay);
    s_standalone_tab = standalone;
    if (runtime) reshade::set_config_value(runtime, "ShaderLabCapture", "StandaloneTab", s_standalone_tab);
    reshade::register_overlay(s_standalone_tab ? "ShaderLab Capture" : nullptr, &on_overlay);
}

bool CaptureManager::is_standalone_tab() const {
    return s_standalone_tab;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule)) {
            return FALSE;
        }
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(&on_begin_effects);
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(&on_finish_effects);
        reshade::register_event<reshade::addon_event::reshade_screenshot>(&on_screenshot);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&on_reloaded_effects);
        reshade::register_event<reshade::addon_event::reshade_overlay>(&on_reshade_overlay_frame);
        reshade::register_overlay(s_standalone_tab ? "ShaderLab Capture" : nullptr, &on_overlay);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_overlay(s_standalone_tab ? "ShaderLab Capture" : nullptr, &on_overlay);
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(&on_reshade_overlay_frame);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(&on_reloaded_effects);
        reshade::unregister_event<reshade::addon_event::reshade_screenshot>(&on_screenshot);
        reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(&on_finish_effects);
        reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(&on_begin_effects);
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
