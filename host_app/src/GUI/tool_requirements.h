#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <filesystem>

namespace gui {

class ToolRequirements {
public:
    static ToolRequirements &get();

    // depth anything v2
    bool is_depth_script_present() const;
    std::filesystem::path find_depth_script_path() const;

    bool is_depth_model_present(const std::string &encoder) const;
    std::filesystem::path find_depth_model_path(const std::string &encoder) const;

    bool is_depth_ready(const std::string &encoder) const {
        return is_depth_script_present() && is_depth_model_present(encoder);
    }

    bool is_depth_downloading() const { return m_depth_downloading.load(); }
    float get_depth_download_progress() const { return m_depth_download_progress.load(); }
    std::string get_depth_download_status() const;
    bool trigger_depth_download(const std::string &encoder);

    // lama inpainting
    bool is_lama_script_present() const;
    std::filesystem::path find_lama_script_path() const;

    bool is_lama_model_present() const;
    std::filesystem::path find_lama_model_path() const;

    bool is_lama_ready() const {
        return is_lama_script_present() && is_lama_model_present();
    }

    bool is_lama_downloading() const { return m_lama_downloading.load(); }
    float get_lama_download_progress() const { return m_lama_download_progress.load(); }
    std::string get_lama_download_status() const;
    bool trigger_lama_download();

    std::vector<std::filesystem::path> get_search_dirs() const;

    ~ToolRequirements();

private:
    ToolRequirements();

    std::atomic<bool> m_depth_downloading{ false };
    std::atomic<float> m_depth_download_progress{ 0.0f };
    std::atomic<bool> m_depth_cancel{ false };
    std::string m_depth_status;
    mutable std::mutex m_depth_mutex;
    std::thread m_depth_thread;

    std::atomic<bool> m_lama_downloading{ false };
    std::atomic<float> m_lama_download_progress{ 0.0f };
    std::atomic<bool> m_lama_cancel{ false };
    std::string m_lama_status;
    mutable std::mutex m_lama_mutex;
    std::thread m_lama_thread;
};

} // namespace gui
