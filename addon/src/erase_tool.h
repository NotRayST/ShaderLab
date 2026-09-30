#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <filesystem>
#include <d3d11.h>
#include <wrl/client.h>
#ifndef ImTextureID
#define ImTextureID ImU64
#endif
#include <imgui.h>
#include <reshade.hpp>
#include "../../common/ipc_protocol.h"

using Microsoft::WRL::ComPtr;

class EraseTool {
public:
    static EraseTool &get();
    ~EraseTool();

    // Mode activation & toggling
    bool is_active() const { return m_active; }
    void set_active(bool active);
    void toggle() { set_active(!m_active); }

    // Mask painting & stroke undo/redo
    void clear_mask();
    bool undo_stroke();
    bool redo_stroke();
    bool can_undo_stroke() const { return !m_undo_stack.empty(); }
    bool can_redo_stroke() const { return !m_redo_stack.empty(); }
    bool has_mask_edits() const { return m_mask_white_px > 0; }
    size_t get_mask_white_px() const { return m_mask_white_px; }
    bool export_mask_png(const std::wstring &path, uint32_t w, uint32_t h, size_t *out_white_px = nullptr);

    // Frame update, brush input, overlay & UI
    void handle_input(SharedControlBlock *b, bool is_processing, bool *out_stroke_finished = nullptr);
    void render_overlay(SharedControlBlock *b, reshade::api::effect_runtime *runtime = nullptr);
    void handle_frame(SharedControlBlock *b, reshade::api::effect_runtime *runtime, const std::wstring &active_path, bool bg_hovered, bool any_active, bool fine);
    void render_ui(SharedControlBlock *b, const std::wstring &active_path);
    void set_ui_rect(float x0, float y0, float x1, float y1) { m_ui_min = ImVec2(x0, y0); m_ui_max = ImVec2(x1, y1); }

    // Tool settings
    float brush_radius = 24.0f;
    bool subtract_mode = false;
    bool auto_apply = true;

    // Backend inpainting & stage history
    bool trigger_erase(const std::wstring &base_image_path, const std::wstring &mask_path);
    bool is_processing() const { return m_processing.load(); }
    float get_progress() const { return m_progress.load(); }
    std::string get_status() const { std::lock_guard<std::mutex> l(m_status_mutex); return m_status; }
    std::string get_last_error() const { std::lock_guard<std::mutex> l(m_status_mutex); return m_last_error; }

    bool ensure_stage_cache_initialized(const std::wstring &base_image_path);
    bool set_stage_index(size_t target_idx);
    bool undo_erase();
    bool redo_erase();
    bool undo_last_erase() { return undo_erase(); }
    bool can_undo_erase() const { std::lock_guard<std::mutex> l(m_stage_mutex); return m_stage_idx > 0 && m_stage_idx < m_stages.size(); }
    bool can_redo_erase() const { std::lock_guard<std::mutex> l(m_stage_mutex); return !m_stages.empty() && (m_stage_idx + 1 < m_stages.size()); }
    size_t get_current_erase_step() const { std::lock_guard<std::mutex> l(m_stage_mutex); return m_stage_idx; }
    size_t get_total_erase_steps() const { std::lock_guard<std::mutex> l(m_stage_mutex); return m_stages.empty() ? 0 : m_stages.size() - 1; }
    void clear_erase_history();

    // Model & daemon worker
    bool is_model_present() const;
    bool is_lama_present() const { return is_model_present(); }
    bool is_migan_present() const { return is_model_present(); }
    bool trigger_model_download();
    bool trigger_lama_download() { return trigger_model_download(); }
    bool trigger_migan_download() { return trigger_model_download(); }
    bool is_downloading() const { return m_downloading.load(); }
    float get_download_progress() const { return m_download_progress.load(); }
    std::string get_download_status() const;

    bool prewarm_worker();
    bool prewarm_lama_worker() { return prewarm_worker(); }
    bool prewarm_migan_worker() { return prewarm_worker(); }
    void stop_worker();
    void stop_lama_worker() { stop_worker(); }
    void stop_migan_worker() { stop_worker(); }
    bool is_worker_ready() const { return m_daemon_ready.load(); }
    bool is_lama_worker_ready() const { return is_worker_ready(); }
    bool is_migan_worker_ready() const { return is_worker_ready(); }

    std::string get_model_name() const;
    std::string get_lama_model_name() const { return get_model_name(); }
    std::string get_migan_model_name() const { return get_model_name(); }
    std::wstring find_lama_script_path() const;
    std::wstring find_migan_script_path() const { return find_lama_script_path(); }
    std::wstring find_lama_model_path() const;
    std::wstring find_migan_model_path() const { return find_lama_model_path(); }
    std::wstring find_python_executable() const;

    void log(int severity, const std::string &msg);

private:
    EraseTool();

    ImVec2 screen_to_image(const ImVec2 &sp, const SharedControlBlock *b, const ImVec2 &disp) const;
    ImVec2 image_to_screen(const ImVec2 &ip, const SharedControlBlock *b, const ImVec2 &disp) const;
    void ensure_mask_size(uint32_t w, uint32_t h);
    void push_undo();
    void recalculate_white_pixels();
    void draw_brush_dab(int cx, int cy, int r, uint8_t val);
    void draw_brush_line(const ImVec2 &p0, const ImVec2 &p1, float radius, bool is_sub);
    void update_gpu_texture(ID3D11Device *dev);
    std::vector<std::filesystem::path> get_base_dirs() const;

    bool m_active = false, m_is_drawing = false, m_last_sub = false, m_mask_dirty = false, m_was_erasing = false;
    ImVec2 m_last_pos{ -1, -1 }, m_ui_min{ 0, 0 }, m_ui_max{ 0, 0 };
    std::vector<uint8_t> m_mask;
    uint32_t m_mask_w = 0, m_mask_h = 0;
    size_t m_mask_white_px = 0;
    std::vector<std::vector<uint8_t>> m_undo_stack, m_redo_stack;

    ComPtr<ID3D11Texture2D> m_mask_tex;
    ComPtr<ID3D11ShaderResourceView> m_mask_srv;
    std::vector<uint32_t> m_gpu_rgba;
    uint32_t m_tex_w = 0, m_tex_h = 0;

    struct EraseStage { std::wstring img, sidecar; };
    mutable std::mutex m_stage_mutex;
    std::wstring m_base_img;
    std::vector<EraseStage> m_stages;
    size_t m_stage_idx = 0;

    std::atomic<bool> m_processing{ false };
    std::atomic<float> m_progress{ -1.0f };
    std::atomic<bool> m_downloading{ false };
    std::atomic<float> m_download_progress{ 0.0f };
    std::atomic<bool> m_cancel_download{ false };
    std::thread m_download_thread;
    mutable std::mutex m_status_mutex;
    std::string m_status, m_last_error, m_download_status;
    std::thread m_worker;

    PROCESS_INFORMATION m_daemon_pi = {};
    HANDLE m_daemon_in = nullptr, m_daemon_out = nullptr;
    std::atomic<bool> m_daemon_ready{ false }, m_daemon_prewarming{ false };
    std::atomic<bool> m_daemon_stopping{ false };
    mutable std::mutex m_daemon_mutex;
    std::thread m_daemon_thread;
    std::condition_variable m_daemon_cv;
    std::string m_daemon_result_line;
    bool m_daemon_has_result = false;

    mutable std::mutex m_cache_mutex;
    mutable std::wstring m_cached_script, m_cached_model, m_cached_py;
};
