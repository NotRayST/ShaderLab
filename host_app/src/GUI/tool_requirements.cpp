#include "tool_requirements.h"
#include <urlmon.h>
#include <system_error>

#pragma comment(lib, "urlmon.lib")

namespace fs = std::filesystem;

namespace gui {

namespace {

class DownloadCallback : public IBindStatusCallback {
public:
    std::atomic<float> *progress_out = nullptr;
    std::atomic<bool> *cancel_flag = nullptr;

    STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *ppv = static_cast<IBindStatusCallback *>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
    STDMETHOD(OnStartBinding)(DWORD, IBinding *) override { return S_OK; }
    STDMETHOD(GetPriority)(LONG *) override { return S_OK; }
    STDMETHOD(OnLowResource)(DWORD) override { return S_OK; }
    STDMETHOD(OnProgress)(ULONG ulProgress, ULONG ulProgressMax, ULONG, LPCWSTR) override {
        if (cancel_flag && cancel_flag->load()) return E_ABORT;
        if (progress_out && ulProgressMax > 0) {
            progress_out->store(static_cast<float>(ulProgress) / static_cast<float>(ulProgressMax));
        }
        return S_OK;
    }
    STDMETHOD(OnStopBinding)(HRESULT, LPCWSTR) override { return S_OK; }
    STDMETHOD(GetBindInfo)(DWORD *grfBINDF, BINDINFO *pbindinfo) override {
        if (!grfBINDF || !pbindinfo) return E_POINTER;
        *grfBINDF = BINDF_ASYNCHRONOUS | BINDF_ASYNCSTORAGE | BINDF_NOWRITECACHE;
        return S_OK;
    }
    STDMETHOD(OnDataAvailable)(DWORD, DWORD, FORMATETC *, STGMEDIUM *) override { return S_OK; }
    STDMETHOD(OnObjectAvailable)(REFIID, IUnknown *) override { return S_OK; }
};

std::wstring to_wstring(const std::string &str) {
    if (str.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (sz <= 1) return {};
    std::wstring out(sz - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, out.data(), sz);
    return out;
}

} // namespace

ToolRequirements &ToolRequirements::get() {
    static ToolRequirements instance;
    return instance;
}

ToolRequirements::ToolRequirements() = default;

ToolRequirements::~ToolRequirements() {
    m_depth_cancel.store(true);
    if (m_depth_thread.joinable()) m_depth_thread.join();

    m_lama_cancel.store(true);
    if (m_lama_thread.joinable()) m_lama_thread.join();
}

std::vector<fs::path> ToolRequirements::get_search_dirs() const {
    std::vector<fs::path> dirs;
    wchar_t exe_buf[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH) > 0) {
        fs::path exe_dir = fs::path(exe_buf).parent_path();
        dirs.push_back(exe_dir);
        dirs.push_back(exe_dir / "..");
    }
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (!ec) {
        dirs.push_back(cwd);
    }
    return dirs;
}

// Depth Anything V2 requirements
bool ToolRequirements::is_depth_script_present() const {
    return !find_depth_script_path().empty();
}

fs::path ToolRequirements::find_depth_script_path() const {
    std::error_code ec;
    for (const auto &d : get_search_dirs()) {
        fs::path p1 = d / "tools" / "depth_anything_v2" / "run_depth_anything.py";
        if (fs::exists(p1, ec)) return p1;
        fs::path p2 = d / "run_depth_anything.py";
        if (fs::exists(p2, ec)) return p2;
    }
    return {};
}

bool ToolRequirements::is_depth_model_present(const std::string &encoder) const {
    return !find_depth_model_path(encoder).empty();
}

fs::path ToolRequirements::find_depth_model_path(const std::string &encoder) const {
    std::error_code ec;
    std::string fname = "depth_anything_v2_" + encoder + ".pth";
    uint64_t min_size = 80000000ULL; // ~80MB minimum for vits
    if (encoder == "vitb") min_size = 300000000ULL;
    else if (encoder == "vitl") min_size = 1000000000ULL;

    for (const auto &d : get_search_dirs()) {
        fs::path candidates[] = {
            d / "common" / fname,
            d / "tools" / "depth_anything_v2" / fname,
            d / fname
        };
        for (const auto &c : candidates) {
            if (fs::exists(c, ec) && fs::file_size(c, ec) >= min_size) {
                return c;
            }
        }
    }
    return {};
}

std::string ToolRequirements::get_depth_download_status() const {
    std::lock_guard<std::mutex> l(m_depth_mutex);
    return m_depth_status;
}

bool ToolRequirements::trigger_depth_download(const std::string &encoder) {
    if (is_depth_model_present(encoder)) return true;
    if (m_depth_downloading.exchange(true)) return false;

    m_depth_download_progress.store(0.0f);
    m_depth_cancel.store(false);
    {
        std::lock_guard<std::mutex> l(m_depth_mutex);
        m_depth_status = "Connecting to download source...";
    }

    if (m_depth_thread.joinable()) {
        m_depth_thread.join();
    }

    m_depth_thread = std::thread([this, encoder]() {
        std::vector<std::string> urls;
        uint64_t expected_size = 0;
        if (encoder == "vitl") {
            urls = {
                "https://huggingface.co/depth-anything/Depth-Anything-V2-Large/resolve/main/depth_anything_v2_vitl.pth",
                "https://hf-mirror.com/depth-anything/Depth-Anything-V2-Large/resolve/main/depth_anything_v2_vitl.pth",
                "https://modelscope.cn/api/v1/models/DepthAnything/Depth-Anything-V2-Large/repo?Revision=master&FilePath=depth_anything_v2_vitl.pth"
            };
            expected_size = 1341395338ULL;
        } else if (encoder == "vitb") {
            urls = {
                "https://huggingface.co/depth-anything/Depth-Anything-V2-Base/resolve/main/depth_anything_v2_vitb.pth",
                "https://hf-mirror.com/depth-anything/Depth-Anything-V2-Base/resolve/main/depth_anything_v2_vitb.pth",
                "https://modelscope.cn/api/v1/models/DepthAnything/Depth-Anything-V2-Base/repo?Revision=master&FilePath=depth_anything_v2_vitb.pth"
            };
            expected_size = 389961218ULL;
        } else {
            urls = {
                "https://huggingface.co/depth-anything/Depth-Anything-V2-Small/resolve/main/depth_anything_v2_vits.pth",
                "https://hf-mirror.com/depth-anything/Depth-Anything-V2-Small/resolve/main/depth_anything_v2_vits.pth",
                "https://modelscope.cn/api/v1/models/DepthAnything/Depth-Anything-V2-Small/repo?Revision=master&FilePath=depth_anything_v2_vits.pth"
            };
            expected_size = 99218434ULL;
        }

        std::vector<fs::path> dirs = get_search_dirs();
        fs::path dest = dirs.empty() ? fs::current_path() / "common" : dirs[0] / "common";
        std::error_code ec;
        fs::create_directories(dest, ec);

        std::string fname = "depth_anything_v2_" + encoder + ".pth";
        fs::path target = dest / fname;
        fs::path tmp = dest / (fname + ".tmp");

        DownloadCallback cb;
        cb.progress_out = &m_depth_download_progress;
        cb.cancel_flag = &m_depth_cancel;

        bool downloaded = false;
        for (const auto &u : urls) {
            if (m_depth_cancel.load()) break;
            m_depth_download_progress.store(0.0f);
            {
                std::lock_guard<std::mutex> l(m_depth_mutex);
                m_depth_status = "Downloading " + fname + "...";
            }
            std::wstring w_url = to_wstring(u);
            if (SUCCEEDED(URLDownloadToFileW(nullptr, w_url.c_str(), tmp.wstring().c_str(), 0, &cb)) &&
                fs::exists(tmp, ec) && fs::file_size(tmp, ec) >= (expected_size * 8 / 10)) {
                fs::rename(tmp, target, ec);
                if (!ec && fs::exists(target, ec)) {
                    downloaded = true;
                    break;
                }
            }
            fs::remove(tmp, ec);
        }

        {
            std::lock_guard<std::mutex> l(m_depth_mutex);
            if (downloaded) {
                m_depth_status = "Model downloaded successfully!";
            } else {
                m_depth_status = "Failed to download model weights";
            }
        }
        m_depth_downloading.store(false);
    });

    return true;
}

// LaMa inpainting requirements
bool ToolRequirements::is_lama_script_present() const {
    return !find_lama_script_path().empty();
}

fs::path ToolRequirements::find_lama_script_path() const {
    std::error_code ec;
    for (const auto &d : get_search_dirs()) {
        fs::path p1 = d / "tools" / "lama" / "run_lama.py";
        if (fs::exists(p1, ec)) return p1;
        fs::path p2 = d / "run_lama.py";
        if (fs::exists(p2, ec)) return p2;
    }
    return {};
}

bool ToolRequirements::is_lama_model_present() const {
    return !find_lama_model_path().empty();
}

fs::path ToolRequirements::find_lama_model_path() const {
    std::error_code ec;
    for (const auto &d : get_search_dirs()) {
        fs::path candidates[] = {
            d / "common" / "big-lama.pt",
            d / "tools" / "lama" / "big-lama.pt",
            d / "big-lama.pt"
        };
        for (const auto &c : candidates) {
            if (fs::exists(c, ec) && fs::file_size(c, ec) >= 100000000ULL) {
                return c;
            }
        }
    }
    return {};
}

std::string ToolRequirements::get_lama_download_status() const {
    std::lock_guard<std::mutex> l(m_lama_mutex);
    return m_lama_status;
}

bool ToolRequirements::trigger_lama_download() {
    if (is_lama_model_present()) return true;
    if (m_lama_downloading.exchange(true)) return false;

    m_lama_download_progress.store(0.0f);
    m_lama_cancel.store(false);
    {
        std::lock_guard<std::mutex> l(m_lama_mutex);
        m_lama_status = "Connecting to download source...";
    }

    if (m_lama_thread.joinable()) {
        m_lama_thread.join();
    }

    m_lama_thread = std::thread([this]() {
        std::vector<fs::path> dirs = get_search_dirs();
        fs::path dest = dirs.empty() ? fs::current_path() / "common" : dirs[0] / "common";
        std::error_code ec;
        fs::create_directories(dest, ec);

        fs::path target = dest / "big-lama.pt";
        fs::path tmp = dest / "big-lama.pt.tmp";

        DownloadCallback cb;
        cb.progress_out = &m_lama_download_progress;
        cb.cancel_flag = &m_lama_cancel;

        bool downloaded = false;
        const wchar_t *urls[] = {
            L"https://github.com/Sanster/models/releases/download/add_big_lama/big-lama.pt",
            L"https://hf-mirror.com/fashn-ai/LaMa/resolve/main/big-lama.pt",
            L"https://huggingface.co/fashn-ai/LaMa/resolve/main/big-lama.pt"
        };

        for (auto u : urls) {
            if (m_lama_cancel.load()) break;
            m_lama_download_progress.store(0.0f);
            {
                std::lock_guard<std::mutex> l(m_lama_mutex);
                m_lama_status = "Downloading LaMa Big HD (~196MB)...";
            }
            if (SUCCEEDED(URLDownloadToFileW(nullptr, u, tmp.wstring().c_str(), 0, &cb)) &&
                fs::exists(tmp, ec) && fs::file_size(tmp, ec) >= 100000000ULL) {
                fs::rename(tmp, target, ec);
                if (!ec && fs::exists(target, ec)) {
                    downloaded = true;
                    break;
                }
            }
            fs::remove(tmp, ec);
        }

        {
            std::lock_guard<std::mutex> l(m_lama_mutex);
            if (downloaded) {
                m_lama_status = "LaMa Big HD downloaded successfully!";
            } else {
                m_lama_status = "Failed to download LaMa Big HD";
            }
        }
        m_lama_downloading.store(false);
    });

    return true;
}

} // namespace gui
