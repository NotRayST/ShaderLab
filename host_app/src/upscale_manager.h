#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gui {
class GuiManager;
}

struct UpscaleModelSpec {
    const char *key = "";
    const char *display_name = "";
    const char *filename = "";
    const char *license = "";
    const char *url = "";
    int native_scale = 4;
    uint64_t expected_min_bytes = 0;
    bool requires_spandrel = false;
};

struct UpscaleSettings {
    int model_idx = 0;       // 0=x4plus, 1=x2plus, 2=anime_6B, 3=RealPLKSR
    int scale_mode = 1;      // 0=2x, 1=4x, 2=Custom
    int custom_width = 2048;
    int custom_height = 2048;
    int tile_mode = 0;       // 0=512 (Auto), 1=256 (Low VRAM), 2=768 (Fast), 3=No Tiling
    float blend_strength = 1.0f; // 0.0f..1.0f (0=bicubic, 1=100% AI)
    bool upscale_depth = true;
    bool upscale_alpha = true;
    bool use_fp16 = true;
    bool lock_aspect = true;
    uint32_t last_src_w = 0;
    uint32_t last_src_h = 0;
};

class UpscaleManager {
public:
    static UpscaleManager &get();

    UpscaleManager(const UpscaleManager &) = delete;
    UpscaleManager &operator=(const UpscaleManager &) = delete;
    UpscaleManager(UpscaleManager &&) = delete;
    UpscaleManager &operator=(UpscaleManager &&) = delete;

    void initialize(const std::filesystem::path &workspace_root);
    void shutdown();

    static const std::vector<UpscaleModelSpec> &get_model_catalog();
    std::filesystem::path get_models_dir() const;
    std::filesystem::path get_model_path(int model_idx) const;
    std::filesystem::path get_script_path() const;
    bool is_model_available(int model_idx) const;

    bool is_running() const { return m_running.load(); }
    bool is_downloading() const { return m_downloading.load(); }
    int get_progress_pct() const { return m_progress_pct.load(); }
    std::string get_status_message() const;
    std::string get_last_error() const;

    void download_model_async(int model_idx);

    bool start_upscale(
        const std::filesystem::path &input_image_path,
        uint32_t src_w,
        uint32_t src_h,
        const std::filesystem::path &input_depth_path = {}
    );

    bool consume_completed_result(
        std::filesystem::path &out_image_path,
        std::filesystem::path &out_depth_path,
        std::string &out_error
    );

    void cancel();

    UpscaleSettings &settings_mut() { return m_settings; }
    const UpscaleSettings &settings() const { return m_settings; }
    bool pop_upscale_request();

    // UI helpers following AGENTS.MD wrapping & alignment rules
    void render_upscale_tab(
        gui::GuiManager &gui,
        bool has_image,
        uint32_t img_w,
        uint32_t img_h,
        bool has_depth,
        bool include_run_button = false
    );
    void render_upscale_bottom_button(gui::GuiManager &gui, bool has_image);

    static bool upscale_depth_sidecar_bilinear(
        const std::filesystem::path &src_depth,
        const std::filesystem::path &dst_depth,
        uint32_t target_w,
        uint32_t target_h,
        std::string &out_error
    );

    void compute_target_dimensions(uint32_t src_w, uint32_t src_h, uint32_t &out_w, uint32_t &out_h) const;

private:
    UpscaleManager() = default;
    ~UpscaleManager();

    std::string find_python_executable() const;
    void set_status(const std::string &msg, int pct = -1);
    void set_error(const std::string &err);
    void join_worker();
    void join_download_worker();

    std::filesystem::path m_workspace_root;
    UpscaleSettings m_settings;

    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_downloading{ false };
    std::atomic<bool> m_cancel_requested{ false };
    std::atomic<int> m_progress_pct{ 0 };
    bool m_ui_req_run = false;

    mutable std::mutex m_state_mutex;
    std::string m_status_message = "Idle";
    std::string m_last_error;

    bool m_has_completed = false;
    bool m_completed_ok = false;
    std::filesystem::path m_completed_image_path;
    std::filesystem::path m_completed_depth_path;
    std::string m_completed_error;

    HANDLE m_active_process = nullptr;
    std::thread m_worker_thread;
    std::thread m_download_thread;
};
