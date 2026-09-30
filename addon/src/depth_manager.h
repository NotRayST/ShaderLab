#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <thread>
#include <filesystem>
#include "../../common/depth_file.h"
#include "../../common/depth_chunk.h"

struct DepthLogEntry {
    std::string timestamp;
    int severity; // 0=info, 1=warn, 2=error, 3=success
    std::string message;
};

class DepthManager {
public:
    static DepthManager &get();

    // attach depth from screenshot
    bool attach_depth_from_image(
        const std::wstring &base_image_path,
        const std::wstring &depth_image_path,
        bool invert,
        float far_plane,
        bool embed_png,
        std::string &out_error
    );

    // one-click depth estimation
    bool trigger_depth_estimate(
        const std::wstring &base_image_path,
        float far_plane = 1000.0f,
        bool embed_png = false,
        const std::string &model_encoder = "vitl",
        int input_size = 2016,
        float gamma = 1.0f,
        float near_threshold = 0.0f,
        float sky_threshold = 0.0f,
        bool edge_refine = false,
        bool invert = false,
        bool smooth_normals = false,
        int smooth_radius = 8,
        float smooth_eps = 1e-3f
    );

    bool is_processing() const { return m_processing.load(); }
    float get_progress() const { return m_progress.load(); }
    std::string get_status() const;
    std::string get_last_error() const;

    // model detection / download
    bool is_model_present(const std::string &encoder = "vits") const;
    bool is_downloading() const { return m_downloading.load(); }
    float get_download_progress() const;
    std::string get_download_status_text() const;
    std::string get_download_error() const;
    bool trigger_model_download(const std::string &encoder = "vits");
    bool trigger_model_download_and_estimate(
        const std::wstring &base_image_path,
        float far_plane = 1000.0f,
        bool embed_png = false,
        const std::string &model_encoder = "vitl",
        int input_size = 2016,
        float gamma = 1.0f,
        float near_threshold = 0.0f,
        float sky_threshold = 0.0f,
        bool edge_refine = false,
        bool invert = false,
        bool smooth_normals = false,
        int smooth_radius = 8,
        float smooth_eps = 1e-3f
    );

    // logging
    void log(int severity, const std::string &msg);
    std::vector<DepthLogEntry> get_logs() const;
    void clear_logs();

    // remove depth chunk or sidecar
    bool remove_depth(const std::wstring &base_image_path, std::string &out_error);

    // export depth buffer to png or sidecar
    bool export_depth(
        const std::wstring &base_image_path,
        const std::wstring &output_path,
        bool as_16bit_png,
        std::string &out_error
    );

    // config / paths
    std::wstring get_python_path() const { return m_python_path; }
    void set_python_path(const std::wstring &path) { m_python_path = path; }

    std::wstring get_model_path() const { return m_model_path; }
    void set_model_path(const std::wstring &path) { m_model_path = path; }

    std::wstring find_python_executable() const;
    std::wstring find_script_path() const;
    std::wstring find_model_path(const std::string &encoder = "vits") const;
    std::vector<std::filesystem::path> get_base_search_dirs() const;

    uint64_t get_expected_model_size(const std::string &encoder) const;
    bool verify_model_file(const std::wstring &path, const std::string &encoder) const;

    // inpainting erase support (forwarded to EraseTool)
    bool is_lama_present() const;
    bool is_migan_present() const { return is_lama_present(); }
    bool trigger_lama_download();
    bool trigger_migan_download() { return trigger_lama_download(); }
    bool trigger_erase(const std::wstring &base_image_path, const std::wstring &mask_path);
    bool undo_erase();
    bool redo_erase();
    bool undo_last_erase() { return undo_erase(); }
    bool can_undo_erase() const;
    bool can_redo_erase() const;
    size_t get_current_erase_step() const;
    size_t get_total_erase_steps() const;
    void clear_erase_history();
    bool ensure_stage_cache_initialized(const std::wstring &base_image_path);
    std::wstring find_lama_script_path() const;
    std::wstring find_migan_script_path() const { return find_lama_script_path(); }
    std::wstring find_lama_model_path() const;
    std::wstring find_migan_model_path() const { return find_lama_model_path(); }
    std::string get_lama_model_name() const;
    std::string get_migan_model_name() const { return get_lama_model_name(); }

    bool provision_portable_python(std::wstring &out_py_exe);
    void invalidate_model_cache();

    bool prewarm_lama_worker();
    bool prewarm_migan_worker() { return prewarm_lama_worker(); }
    void stop_lama_worker();
    void stop_migan_worker() { stop_lama_worker(); }
    bool is_lama_worker_ready() const;
    bool is_migan_worker_ready() const { return is_lama_worker_ready(); }

private:
    DepthManager();
    ~DepthManager();

    std::atomic<bool> m_deps_verified{ false };

    std::atomic<bool> m_processing{ false };
    std::atomic<float> m_progress{ -1.0f };
    mutable std::mutex m_status_mutex;
    std::string m_status_text;
    std::string m_last_error;
    std::thread m_worker;

    // download state
    std::atomic<bool> m_downloading{ false };
    std::atomic<uint64_t> m_download_received{ 0 };
    std::atomic<uint64_t> m_download_total{ 0 };
    std::string m_download_status_text;
    std::string m_download_error;
    std::thread m_download_worker;

    struct PendingDepthEstimate {
        bool active{ false };
        std::wstring base_image_path;
        float far_plane{ 1000.0f };
        bool embed_png{ false };
        std::string model_encoder{ "vitl" };
        int input_size{ 2016 };
        float gamma{ 1.0f };
        float near_threshold{ 0.0f };
        float sky_threshold{ 0.0f };
        bool edge_refine{ false };
        bool invert{ false };
        bool smooth_normals{ false };
        int smooth_radius{ 8 };
        float smooth_eps{ 1e-3f };
    };
    PendingDepthEstimate m_pending_estimate;
    mutable std::mutex m_pending_mutex;

    std::vector<DepthLogEntry> m_logs;
    std::wstring m_python_path;
    std::wstring m_model_path;

    mutable std::mutex m_cache_mutex;
    mutable std::unordered_map<std::string, std::wstring> m_cached_model_paths;
    mutable std::wstring m_cached_script_path;
    mutable std::wstring m_cached_python_path;
};
