#pragma once
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <fstream>
#include <chrono>

class CaptureManager {
public:
    static CaptureManager& get();

    void init();
    void log(const std::string& level, const std::string& msg);

    void on_reshade_begin_effects(reshade::api::effect_runtime* runtime, reshade::api::command_list* cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb);
    void on_reshade_finish_effects(reshade::api::effect_runtime* runtime, reshade::api::command_list* cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb);
    void on_reshade_screenshot(reshade::api::effect_runtime* runtime, const char* path);
    void on_reshade_reloaded_effects(reshade::api::effect_runtime* runtime);
    void on_draw_overlay(reshade::api::effect_runtime* runtime);
    void on_reshade_overlay_frame(reshade::api::effect_runtime* runtime);

    void set_standalone_tab(reshade::api::effect_runtime* runtime, bool standalone);
    bool is_standalone_tab() const;

    void set_enabled(reshade::api::effect_runtime* runtime, bool enabled);
    bool is_enabled() const { return m_enabled; }

    bool embed_depth_in_png(reshade::api::effect_runtime* runtime, const std::string& png_path);
    bool save_depth_sidecar(reshade::api::effect_runtime* runtime, const std::string& sidecar_path);

private:
    CaptureManager() = default;
    ~CaptureManager() = default;
    CaptureManager(const CaptureManager&) = delete;
    CaptureManager& operator=(const CaptureManager&) = delete;

    std::string get_log_file_path() const;
    void ensure_shader_file(reshade::api::effect_runtime* runtime);
    void load_notice_state(reshade::api::effect_runtime* runtime);

    std::mutex m_log_mutex;
    std::ofstream m_log_file;

    // harvested texture resource from capture shader
    reshade::api::resource m_depth_resource{ 0 };
    reshade::api::resource_desc m_depth_desc{};
    std::atomic<bool> m_technique_found{ false };

    // diagnostics & ui state
    bool m_enabled{ true };
    int  m_depth_bit_depth{ 1 }; // 0 = 32-bit float, 1 = 16-bit int, 2 = 8-bit int, 3 = 4-bit int
    std::atomic<bool> m_last_capture_success{ false };
    std::string m_last_capture_status{ "Ready - Press ReShade Screenshot key" };
    uint32_t m_last_capture_w{ 0 };
    uint32_t m_last_capture_h{ 0 };
    float m_last_min_depth{ 0.0f };
    float m_last_max_depth{ 0.0f };
    bool m_last_is_flat{ false };
    std::string m_last_saved_png_path;
    std::string m_last_saved_sidecar_path;

    // post-capture prompt state
    bool m_notice_checked{ false };
    bool m_notice_dismissed{ false };
    bool m_notice_open{ false };       // notice window visible this session
    std::wstring m_toast_exe;
    std::chrono::steady_clock::time_point m_toast_start{};
};
