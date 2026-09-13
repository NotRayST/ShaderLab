#pragma once
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
    bool trigger_ai_depth(
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

    bool is_ai_running() const { return m_ai_running.load(); }
    float get_ai_progress() const { return m_ai_progress.load(); }
    std::string get_ai_status() const;
    std::string get_ai_last_error() const;

    // model detection / download
    bool is_model_present(const std::string &encoder = "vits") const;
    bool is_downloading() const { return m_downloading.load(); }
    float get_download_progress() const;
    std::string get_download_status_text() const;
    std::string get_download_error() const;
    bool trigger_model_download(const std::string &encoder = "vits");

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

    bool provision_portable_python(std::wstring &out_py_exe);
    void invalidate_model_cache();

private:
    DepthManager();
    ~DepthManager();

    std::atomic<bool> m_deps_verified{ false };

    std::atomic<bool> m_ai_running{ false };
    std::atomic<float> m_ai_progress{ -1.0f };
    mutable std::mutex m_status_mutex;
    std::string m_ai_status;
    std::string m_ai_last_error;
    std::thread m_ai_worker;

    // download state
    std::atomic<bool> m_downloading{ false };
    std::atomic<uint64_t> m_download_received{ 0 };
    std::atomic<uint64_t> m_download_total{ 0 };
    std::string m_download_status_text;
    std::string m_download_error;
    std::thread m_download_worker;

    std::vector<DepthLogEntry> m_logs;
    std::wstring m_python_path;
    std::wstring m_model_path;

    mutable std::mutex m_cache_mutex;
    mutable std::unordered_map<std::string, std::wstring> m_cached_model_paths;
    mutable std::wstring m_cached_script_path;
    mutable std::wstring m_cached_python_path;
};
