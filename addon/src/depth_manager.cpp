#include "depth_manager.h"
#include "job_queue.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <functional>
#include <windows.h>
#include <shellapi.h>
#include <wininet.h>
#include <urlmon.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "urlmon.lib")

#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"
#include "../../third_party/stb/stb_image_write.h"

namespace fs = std::filesystem;

class DownloadCallback : public IBindStatusCallback {
public:
    DownloadCallback(std::function<void(uint64_t, uint64_t)> cb, std::function<bool()> cancel_check)
        : m_cb(std::move(cb)), m_cancel(std::move(cancel_check)), m_ref(1) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *ppvObject = static_cast<IBindStatusCallback *>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG res = --m_ref;
        if (res == 0) delete this;
        return res;
    }

    // IBindStatusCallback
    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG ulProgress, ULONG ulProgressMax, ULONG, LPCWSTR) override {
        if (m_cancel && m_cancel()) {
            return E_ABORT;
        }
        if (m_cb) {
            m_cb(static_cast<uint64_t>(ulProgress), static_cast<uint64_t>(ulProgressMax));
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD *grfBINDF, BINDINFO * /*pbindinfo*/) override {
        if (grfBINDF) {
            *grfBINDF = BINDF_ASYNCHRONOUS | BINDF_ASYNCSTORAGE | BINDF_PULLDATA | BINDF_GETNEWESTVERSION | BINDF_NOWRITECACHE;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC *, STGMEDIUM *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown *) override { return S_OK; }

private:
    std::function<void(uint64_t, uint64_t)> m_cb;
    std::function<bool()> m_cancel;
    std::atomic<ULONG> m_ref{ 1 };
};

#include "../../common/str_utils.h"

using str_utils::wide_to_utf8;
using str_utils::utf8_to_wide;

DepthManager &DepthManager::get() {
    static DepthManager instance;
    return instance;
}

DepthManager::DepthManager() {
}

DepthManager::~DepthManager() {
    if (m_ai_worker.joinable()) {
        m_ai_worker.join();
    }
    if (m_download_worker.joinable()) {
        m_download_worker.join();
    }
}

void DepthManager::log(int severity, const std::string &msg) {
    try {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::stringstream ss;
        struct tm buf;
        localtime_s(&buf, &in_time_t);
        ss << std::put_time(&buf, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
        std::string ts = ss.str();

        const char *sev_str = (severity == 1) ? "WARN" : (severity == 2) ? "ERROR" : (severity == 3) ? "SUCCESS" : "INFO";
        std::string line = "[" + ts + "] [" + sev_str + "] " + msg;

        {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            m_logs.push_back({ ts, severity, msg });
        }

        wchar_t exe_path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
            fs::path log_file = fs::path(exe_path).parent_path() / "ShaderLabDepth.log";
            std::ofstream ofs(log_file, std::ios::out | std::ios::app);
            if (ofs.is_open()) {
                ofs << line << "\n";
            }
        }

        JobQueueManager::get().add_log(utf8_to_wide(line.c_str()));
        OutputDebugStringA((line + "\n").c_str());
    } catch (...) {
    }
}

std::vector<DepthLogEntry> DepthManager::get_logs() const {
    std::lock_guard<std::mutex> lock(m_status_mutex);
    return m_logs;
}

void DepthManager::clear_logs() {
    std::lock_guard<std::mutex> lock(m_status_mutex);
    m_logs.clear();
}

std::string DepthManager::get_ai_status() const {
    std::lock_guard<std::mutex> lock(m_status_mutex);
    return m_ai_status;
}

std::string DepthManager::get_ai_last_error() const {
    std::lock_guard<std::mutex> lock(m_status_mutex);
    return m_ai_last_error;
}

uint64_t DepthManager::get_expected_model_size(const std::string &encoder) const {
    std::string enc = encoder;
    std::transform(enc.begin(), enc.end(), enc.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    if (enc == "vitl") return 1341395338ULL;
    if (enc == "vitb") return 389961218ULL;
    if (enc == "vits") return 99218434ULL;
    return 50000000ULL;
}

bool DepthManager::verify_model_file(const std::wstring &path, const std::string &encoder) const {
    if (path.empty()) return false;
    std::error_code ec;
    if (!fs::exists(path, ec) || ec || !fs::is_regular_file(path, ec) || ec) {
        return false;
    }

    uintmax_t sz = fs::file_size(path, ec);
    if (ec) return false;

    uint64_t expected = get_expected_model_size(encoder);
    if (sz < static_cast<uintmax_t>(expected * 0.95)) {
        return false;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    char magic[4] = {};
    ifs.read(magic, 4);
    if (ifs.gcount() < 2) return false;

    bool is_zip = (magic[0] == 'P' && magic[1] == 'K');
    bool is_pickle = (static_cast<unsigned char>(magic[0]) == 0x80);
    return is_zip || is_pickle;
}

std::vector<fs::path> DepthManager::get_base_search_dirs() const {
    std::vector<fs::path> dirs;
    std::error_code ec;

    HMODULE hMod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&DepthManager::get, &hMod) && hMod) {
        wchar_t mod_path[MAX_PATH] = {};
        if (GetModuleFileNameW(hMod, mod_path, MAX_PATH) > 0) {
            fs::path p = fs::path(mod_path).parent_path();
            if (fs::exists(p, ec) && !ec) {
                dirs.push_back(p);
            }
        }
    }

    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        fs::path p = fs::path(exe_path).parent_path();
        if (fs::exists(p, ec) && !ec) {
            bool dup = false;
            for (const auto &d : dirs) {
                if (fs::equivalent(d, p, ec) && !ec) { dup = true; break; }
            }
            if (!dup) dirs.push_back(p);
        }
    }

    fs::path cwd = fs::current_path(ec);
    if (!ec && !cwd.empty() && fs::exists(cwd, ec)) {
        bool dup = false;
        for (const auto &d : dirs) {
            if (fs::equivalent(d, cwd, ec) && !ec) { dup = true; break; }
        }
        if (!dup) dirs.push_back(cwd);
    }

    wchar_t user_prof[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", user_prof, MAX_PATH) > 0) {
        fs::path dl = fs::path(user_prof) / "Downloads";
        if (fs::exists(dl, ec) && !ec) {
            dirs.push_back(dl);
        }
    }

    return dirs;
}

bool DepthManager::is_model_present(const std::string &encoder) const {
    try {
        std::wstring p = find_model_path(encoder);
        return verify_model_file(p, encoder);
    } catch (...) {
        return false;
    }
}

float DepthManager::get_download_progress() const {
    uint64_t total = m_download_total.load();
    if (total == 0) return 0.0f;
    return std::clamp(static_cast<float>(m_download_received.load()) / static_cast<float>(total), 0.0f, 1.0f);
}

std::string DepthManager::get_download_status_text() const {
    std::lock_guard<std::mutex> lock(m_status_mutex);
    return m_download_status_text;
}

std::string DepthManager::get_download_error() const {
    std::lock_guard<std::mutex> lock(m_status_mutex);
    return m_download_error;
}

bool DepthManager::trigger_model_download(const std::string &encoder) {
    if (m_downloading.load()) {
        log(1, "Download already in progress...");
        return false;
    }

    if (m_download_worker.joinable()) {
        m_download_worker.join();
    }

    m_downloading = true;
    m_download_received = 0;
    m_download_total = 0;
    m_download_status_text = "Connecting to Hugging Face...";
    m_download_error.clear();

    log(0, "Initiating model download for encoder: " + encoder);

    m_download_worker = std::thread([this, encoder]() {
        try {
            std::vector<std::string> urls;
            uint64_t expected_size = 0;
            if (encoder == "vitl") {
                urls = {
                    "https://huggingface.co/depth-anything/Depth-Anything-V2-Large/resolve/main/depth_anything_v2_vitl.pth",
                    "https://hf-mirror.com/depth-anything/Depth-Anything-V2-Large/resolve/main/depth_anything_v2_vitl.pth",
                    "https://modelscope.cn/api/v1/models/DepthAnything/Depth-Anything-V2-Large/repo?Revision=master&FilePath=depth_anything_v2_vitl.pth"
                };
                expected_size = 1341395338;
            } else if (encoder == "vitb") {
                urls = {
                    "https://huggingface.co/depth-anything/Depth-Anything-V2-Base/resolve/main/depth_anything_v2_vitb.pth",
                    "https://hf-mirror.com/depth-anything/Depth-Anything-V2-Base/resolve/main/depth_anything_v2_vitb.pth",
                    "https://modelscope.cn/api/v1/models/DepthAnything/Depth-Anything-V2-Base/repo?Revision=master&FilePath=depth_anything_v2_vitb.pth"
                };
                expected_size = 389961218;
            } else {
                urls = {
                    "https://huggingface.co/depth-anything/Depth-Anything-V2-Small/resolve/main/depth_anything_v2_vits.pth",
                    "https://hf-mirror.com/depth-anything/Depth-Anything-V2-Small/resolve/main/depth_anything_v2_vits.pth",
                    "https://modelscope.cn/api/v1/models/DepthAnything/Depth-Anything-V2-Small/repo?Revision=master&FilePath=depth_anything_v2_vits.pth"
                };
                expected_size = 99218434;
            }

            std::vector<fs::path> search_dirs = get_base_search_dirs();
            fs::path dest_dir;
            std::error_code ec;
            for (const auto &d : search_dirs) {
                if (fs::exists(d / "common", ec) && !ec) {
                    dest_dir = d / "common";
                    break;
                }
            }
            if (dest_dir.empty()) {
                dest_dir = (!search_dirs.empty()) ? (search_dirs[0] / "common") : (fs::current_path() / "common");
            }
            if (!fs::exists(dest_dir, ec)) {
                fs::create_directories(dest_dir, ec);
            }

            std::string model_fname = "depth_anything_v2_" + encoder + ".pth";
            fs::path dest_file = dest_dir / model_fname;
            fs::path temp_file = dest_dir / (model_fname + ".tmp");

            bool success = false;
            for (size_t url_idx = 0; url_idx < urls.size(); ++url_idx) {
                const std::string &url = urls[url_idx];
                std::wstring url_w = utf8_to_wide(url.c_str());

                log(0, "Connecting to download source (" + std::to_string(url_idx + 1) + "/" + std::to_string(urls.size()) + ") for " + model_fname + "...");
                m_download_total = expected_size;
                m_download_received = 0;

                auto *cb = new DownloadCallback(
                    [this, expected_size](uint64_t received, uint64_t total) {
                        if (total == 0) total = expected_size;
                        m_download_total = total;
                        m_download_received = received;
                        float mb_read = static_cast<float>(received) / (1024.0f * 1024.0f);
                        float mb_tot = static_cast<float>(total) / (1024.0f * 1024.0f);
                        int pct = (total > 0) ? static_cast<int>((received * 100) / total) : 0;
                        char status_buf[128];
                        snprintf(status_buf, sizeof(status_buf), "%.1f MB / %.1f MB (%d%%)", mb_read, mb_tot, pct);
                        std::lock_guard<std::mutex> lock(m_status_mutex);
                        m_download_status_text = status_buf;
                    },
                    [this]() { return !m_downloading.load(); }
                );

                fs::remove(temp_file, ec);
                HRESULT hr = URLDownloadToFileW(nullptr, url_w.c_str(), temp_file.wstring().c_str(), 0, cb);
                cb->Release();

                uint64_t min_acceptable = static_cast<uint64_t>(expected_size * 0.95);
                if (SUCCEEDED(hr) && fs::exists(temp_file, ec) && fs::file_size(temp_file, ec) >= min_acceptable) {
                    fs::rename(temp_file, dest_file, ec);
                    if (ec) {
                        fs::copy_file(temp_file, dest_file, fs::copy_options::overwrite_existing, ec);
                        fs::remove(temp_file, ec);
                    }
                    log(3, "Model weights downloaded successfully: " + model_fname);
                    std::lock_guard<std::mutex> lock(m_status_mutex);
                    m_download_status_text = "Download complete!";
                    m_download_error.clear();
                    success = true;
                    break;
                } else {
                    log(1, "Source " + std::to_string(url_idx + 1) + " failed or returned incomplete file. Trying next mirror...");
                    fs::remove(temp_file, ec);
                }
            }

            if (!success) {
                std::lock_guard<std::mutex> lock(m_status_mutex);
                m_download_error = "All download sources failed for " + model_fname;
                log(2, m_download_error);
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            m_download_error = std::string("Download error: ") + e.what();
            log(2, m_download_error);
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            m_download_error = "Download error: unexpected exception";
            log(2, m_download_error);
        }
        invalidate_model_cache();
        m_downloading = false;
    });

    return true;
}

void DepthManager::invalidate_model_cache() {
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    m_cached_model_paths.clear();
    m_cached_script_path.clear();
    m_cached_python_path.clear();
}

bool DepthManager::provision_portable_python(std::wstring &out_py_exe) {
    try {
        fs::path base_app_dir = fs::current_path();
        wchar_t exe_path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
            base_app_dir = fs::path(exe_path).parent_path();
        } else {
            wchar_t mod_path[MAX_PATH] = {};
            HMODULE hMod = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&DepthManager::get, &hMod) && hMod) {
                if (GetModuleFileNameW(hMod, mod_path, MAX_PATH) > 0) {
                    base_app_dir = fs::path(mod_path).parent_path();
                }
            }
        }

        fs::path py_dir = base_app_dir / "tools" / "python";
        fs::path py_exe = py_dir / "python.exe";
        std::error_code ec;

        if (fs::exists(py_exe, ec) && !ec && fs::file_size(py_exe, ec) > 1024) {
            out_py_exe = py_exe.wstring();
            return true;
        }

        fs::create_directories(py_dir, ec);

        log(0, "Auto-provisioning portable Python 3.11 embed environment into " + wide_to_utf8(py_dir.wstring().c_str()));
        {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            m_ai_status = "Downloading portable Python (~11MB)...";
        }

        // download python embed zip
        fs::path zip_path = py_dir / "python_embed.zip";
        std::vector<std::wstring> py_urls = {
            L"https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip",
            L"https://npmmirror.com/mirrors/python/3.11.9/python-3.11.9-embed-amd64.zip"
        };

        bool dl_ok = false;
        for (const auto &u : py_urls) {
            fs::remove(zip_path, ec);
            HRESULT hr = URLDownloadToFileW(nullptr, u.c_str(), zip_path.wstring().c_str(), 0, nullptr);
            if (SUCCEEDED(hr) && fs::exists(zip_path, ec) && fs::file_size(zip_path, ec) > 5000000) {
                dl_ok = true;
                break;
            }
        }

        if (!dl_ok) {
            log(2, "Failed to download portable Python embed zip");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            m_ai_status = "Extracting portable Python...";
        }

        std::wstring extract_cmd = L"tar.exe -xf \"" + zip_path.wstring() + L"\" -C \"" + py_dir.wstring() + L"\"";
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> cmd_buf(extract_cmd.begin(), extract_cmd.end());
        cmd_buf.push_back(L'\0');

        bool extracted = false;
        if (CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 30000);
            DWORD exit_code = 1;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            extracted = (exit_code == 0 && fs::exists(py_exe, ec));
        }

        fs::remove(zip_path, ec);

        if (!extracted) {
            log(2, "Failed to extract portable Python embed zip");
            return false;
        }

        fs::path pth_file = py_dir / "python311._pth";
        if (fs::exists(pth_file, ec)) {
            std::string content;
            std::ifstream ifs(pth_file);
            if (ifs.is_open()) {
                content.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                ifs.close();
            }
            size_t pos = content.find("#import site");
            if (pos != std::string::npos) {
                content.replace(pos, 12, "import site\n.\nLib\\site-packages\n..\\depth_anything_v2\\site-packages");
            } else if (content.find("import site") == std::string::npos) {
                content += "\nimport site\n.\nLib\\site-packages\n..\\depth_anything_v2\\site-packages\n";
            }
            std::ofstream ofs(pth_file);
            if (ofs.is_open()) {
                ofs << content;
                ofs.close();
            }
        }

        // download get-pip.py and bootstrap pip
        {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            m_ai_status = "Bootstrapping portable pip (~2MB)...";
        }

        fs::path get_pip_path = py_dir / "get-pip.py";
        std::vector<std::wstring> pip_urls = {
            L"https://bootstrap.pypa.io/get-pip.py",
            L"https://raw.githubusercontent.com/pypa/get-pip/main/public/get-pip.py"
        };

        bool get_pip_ok = false;
        for (const auto &u : pip_urls) {
            fs::remove(get_pip_path, ec);
            HRESULT hr = URLDownloadToFileW(nullptr, u.c_str(), get_pip_path.wstring().c_str(), 0, nullptr);
            if (SUCCEEDED(hr) && fs::exists(get_pip_path, ec) && fs::file_size(get_pip_path, ec) > 500000) {
                get_pip_ok = true;
                break;
            }
        }

        if (get_pip_ok) {
            std::wstring pip_boot_cmd = L"\"" + py_exe.wstring() + L"\" \"" + get_pip_path.wstring() + L"\" --no-warn-script-location --no-setuptools --no-wheel";
            STARTUPINFOW pip_si = {};
            pip_si.cb = sizeof(pip_si);
            pip_si.dwFlags = STARTF_USESHOWWINDOW;
            pip_si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pip_pi = {};
            std::vector<wchar_t> pip_buf(pip_boot_cmd.begin(), pip_boot_cmd.end());
            pip_buf.push_back(L'\0');

            if (CreateProcessW(nullptr, pip_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &pip_si, &pip_pi)) {
                WaitForSingleObject(pip_pi.hProcess, 60000);
                CloseHandle(pip_pi.hProcess);
                CloseHandle(pip_pi.hThread);
            }
            fs::remove(get_pip_path, ec);
        }

        if (fs::exists(py_exe, ec)) {
            out_py_exe = py_exe.wstring();
            log(3, "Portable Python provisioned successfully at: " + wide_to_utf8(out_py_exe.c_str()));
            return true;
        }

        return false;
    } catch (const std::exception &e) {
        log(2, "Error provisioning portable Python: " + std::string(e.what()));
        return false;
    }
}

std::wstring DepthManager::find_python_executable() const {
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    try {
        std::error_code ec;
        if (!m_cached_python_path.empty() && fs::exists(m_cached_python_path, ec) && !ec) {
            return m_cached_python_path;
        }

        auto is_valid_python = [](const fs::path &p) -> bool {
            std::error_code err;
            if (!fs::exists(p, err) || err) return false;
            if (!fs::is_regular_file(p, err) || err) return false;
            uintmax_t sz = fs::file_size(p, err);
            if (err || sz < 1024) return false;
            std::wstring pw = p.wstring();
            if (pw.find(L"WindowsApps") != std::wstring::npos) return false;
            return true;
        };

        std::vector<fs::path> base_dirs = get_base_search_dirs();
        for (const auto &b : base_dirs) {
            fs::path cur = b;
            for (int i = 0; i <= 6; ++i) {
                const fs::path local_candidates[] = {
                    cur / "tools" / "python" / "python.exe",
                    cur / "python" / "python.exe",
                    cur / "tools" / "depth_anything_v2" / "venv" / "Scripts" / "python.exe",
                    cur / "tools" / "depth_anything_v2" / "python" / "python.exe",
                    cur / "test" / "tools" / "python" / "python.exe",
                    cur / "venv" / "Scripts" / "python.exe",
                    cur / ".venv" / "Scripts" / "python.exe"
                };
                for (const auto &p : local_candidates) {
                    if (is_valid_python(p)) {
                        m_cached_python_path = p.wstring();
                        return m_cached_python_path;
                    }
                }
                if (!cur.has_parent_path() || cur == cur.parent_path()) break;
                cur = cur.parent_path();
            }
        }

        wchar_t out_path[MAX_PATH] = {};
        if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, out_path, nullptr) > 0) {
            if (is_valid_python(out_path)) {
                m_cached_python_path = out_path;
                return m_cached_python_path;
            }
        }
        if (SearchPathW(nullptr, L"py.exe", nullptr, MAX_PATH, out_path, nullptr) > 0) {
            if (is_valid_python(out_path)) {
                m_cached_python_path = out_path;
                return m_cached_python_path;
            }
        }

        const wchar_t *common_paths[] = {
            L"C:\\Python314\\python.exe",
            L"C:\\Python313\\python.exe",
            L"C:\\Python312\\python.exe",
            L"C:\\Python311\\python.exe",
            L"C:\\Python310\\python.exe",
            L"C:\\Windows\\py.exe"
        };
        for (const auto *p : common_paths) {
            if (is_valid_python(p)) {
                m_cached_python_path = p;
                return m_cached_python_path;
            }
        }

        wchar_t local_app_data[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH) > 0) {
            fs::path p_root = fs::path(local_app_data) / "Programs" / "Python";
            std::error_code root_ec;
            if (fs::exists(p_root, root_ec) && !root_ec) {
                fs::directory_iterator it(p_root, fs::directory_options::skip_permission_denied, root_ec);
                fs::directory_iterator end_it;
                while (!root_ec && it != end_it) {
                    try {
                        fs::path exe = it->path() / "python.exe";
                        if (is_valid_python(exe)) {
                            m_cached_python_path = exe.wstring();
                            return m_cached_python_path;
                        }
                    } catch (...) {
                    }
                    it.increment(root_ec);
                }
            }
        }

        // check program files
        wchar_t prog_files[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"ProgramFiles", prog_files, MAX_PATH) > 0) {
            fs::path p_pf = fs::path(prog_files) / "Python";
            std::error_code pf_ec;
            if (fs::exists(p_pf, pf_ec) && !pf_ec) {
                fs::directory_iterator it(p_pf, fs::directory_options::skip_permission_denied, pf_ec);
                fs::directory_iterator end_it;
                while (!pf_ec && it != end_it) {
                    try {
                        fs::path exe = it->path() / "python.exe";
                        if (is_valid_python(exe)) {
                            m_cached_python_path = exe.wstring();
                            return m_cached_python_path;
                        }
                    } catch (...) {
                    }
                    it.increment(pf_ec);
                }
            }
        }
    } catch (...) {
        // catch all std::filesystem or other exceptions
    }

    m_cached_python_path.clear();
    return m_cached_python_path;
}

std::wstring DepthManager::find_script_path() const {
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    try {
        std::error_code ec;
        if (!m_cached_script_path.empty() && fs::exists(m_cached_script_path, ec) && !ec) {
            return m_cached_script_path;
        }

        std::vector<fs::path> base_dirs = get_base_search_dirs();
        for (const auto &base : base_dirs) {
            fs::path cur = base;
            for (int i = 0; i <= 6; ++i) {
                const fs::path script_candidates[] = {
                    cur / "tools" / "depth_anything_v2" / "run_depth_anything.py",
                    cur / "depth_anything_v2" / "run_depth_anything.py",
                    cur / "run_depth_anything.py",
                    cur / "test" / "tools" / "depth_anything_v2" / "run_depth_anything.py"
                };
                for (const auto &p : script_candidates) {
                    if (fs::exists(p, ec) && !ec && fs::file_size(p, ec) > 100) {
                        fs::path abs_p = fs::absolute(p, ec);
                        m_cached_script_path = (!ec && !abs_p.empty()) ? abs_p.wstring() : p.wstring();
                        return m_cached_script_path;
                    }
                }
                if (!cur.has_parent_path() || cur == cur.parent_path()) break;
                cur = cur.parent_path();
            }
        }
    } catch (...) {
    }

    m_cached_script_path = L"";
    return m_cached_script_path;
}

std::wstring DepthManager::find_model_path(const std::string &encoder) const {
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    try {
        std::error_code ec;
        auto it = m_cached_model_paths.find(encoder);
        if (it != m_cached_model_paths.end() && !it->second.empty() && verify_model_file(it->second, encoder)) {
            return it->second;
        }

        std::wstring enc_w = utf8_to_wide(encoder.c_str());
        std::transform(enc_w.begin(), enc_w.end(), enc_w.begin(), ::towlower);
        std::wstring model_fname = L"depth_anything_v2_" + enc_w + L".pth";

        std::vector<std::wstring> names = {
            model_fname,
            L"depth_anything_v2" + enc_w + L".pth",
            L"depth anything V2 " + enc_w + L".pth"
        };
        if (enc_w == L"vits") {
            names.push_back(L"depth_anything_v2.pth");
            names.push_back(L"depth anything V2.pth");
        }

        std::vector<fs::path> base_dirs = get_base_search_dirs();
        for (const auto &b : base_dirs) {
            fs::path cur = b;
            for (int depth = 0; depth <= 6; ++depth) {
                for (const auto &n : names) {
                    const fs::path candidates[] = {
                        cur / "common" / n,
                        cur / n,
                        cur / "tools" / "depth_anything_v2" / n,
                        cur / "test" / "common" / n,
                        cur / "trash" / "backup_models" / n
                    };
                    for (const auto &cand : candidates) {
                        if (fs::exists(cand, ec) && !ec && verify_model_file(cand.wstring(), encoder)) {
                            fs::path abs_p = fs::absolute(cand, ec);
                            std::wstring resolved = (!ec && !abs_p.empty()) ? abs_p.wstring() : cand.wstring();
                            m_cached_model_paths[encoder] = resolved;
                            return resolved;
                        }
                    }
                }
                if (!cur.has_parent_path() || cur == cur.parent_path()) break;
                cur = cur.parent_path();
            }
        }
    } catch (...) {
    }

    m_cached_model_paths[encoder] = L"";
    return L"";
}

bool DepthManager::attach_depth_from_image(
    const std::wstring &base_image_path,
    const std::wstring &depth_image_path,
    bool invert,
    float far_plane,
    bool embed_png,
    std::string &out_error
) {
    if (base_image_path.empty() || !fs::exists(base_image_path)) {
        out_error = "Active image not found";
        log(2, "Attach Depth Failed: active image not found");
        return false;
    }
    if (depth_image_path.empty() || !fs::exists(depth_image_path)) {
        out_error = "Depth image file not found";
        log(2, "Attach Depth Failed: depth image file not found");
        return false;
    }

    log(0, "Attaching depth from image: " + wide_to_utf8(depth_image_path.c_str()));

    // load depth image
    std::string depth_u8 = wide_to_utf8(depth_image_path.c_str());
    int dw = 0, dh = 0, dcomp = 0;
    
    // check if 16-bit
    bool is_16bit = stbi_is_16_bit(depth_u8.c_str()) != 0;
    std::vector<float> linear_floats;

    if (is_16bit) {
        stbi_us *data16 = stbi_load_16(depth_u8.c_str(), &dw, &dh, &dcomp, 1);
        if (!data16) {
            out_error = "Failed to load 16-bit depth image";
            log(2, out_error);
            return false;
        }
        linear_floats.resize(dw * dh);
        for (int i = 0; i < dw * dh; ++i) {
            float val = static_cast<float>(data16[i]) / 65535.0f;
            linear_floats[i] = invert ? (1.0f - val) : val;
        }
        stbi_image_free(data16);
    } else {
        stbi_uc *data8 = stbi_load(depth_u8.c_str(), &dw, &dh, &dcomp, 1);
        if (!data8) {
            out_error = "Failed to load depth image";
            log(2, out_error);
            return false;
        }
        linear_floats.resize(dw * dh);
        for (int i = 0; i < dw * dh; ++i) {
            float val = static_cast<float>(data8[i]) / 255.0f;
            linear_floats[i] = invert ? (1.0f - val) : val;
        }
        stbi_image_free(data8);
    }

    bool is_flat = false;
    float min_v = 1.0f, max_v = 0.0f;
    depth_file::sanitize_and_analyze(linear_floats.data(), linear_floats.size(), is_flat, min_v, max_v);

    fs::path base_p = fs::absolute(base_image_path);
    std::string ext = base_p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)::tolower(c); });

    if (embed_png && ext == ".png") {
        DepthMapHeader header = {};
        header.magic = kDepthMagicSLD1;
        header.version = kDepthVersion1;
        header.encoding = 0; // R32F_Deflate
        header.width = static_cast<uint32_t>(dw);
        header.height = static_cast<uint32_t>(dh);
        header.flags = is_flat ? 0 : kDepthFlagValid;
        header.near_plane = 1.0f;
        header.far_plane = (far_plane > 0.0f) ? far_plane : 1000.0f;
        header.raw_byte_size = static_cast<uint32_t>(dw * dh * sizeof(float));
        header.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        strcpy_s(header.game_name, "ShaderLabDepth");

        std::string base_u8 = wide_to_utf8(base_p.wstring().c_str());
        if (!depth_chunk::inject_sldp(base_u8, header, linear_floats.data(), linear_floats.size(), out_error)) {
            log(2, "Failed to inject slDp chunk into PNG: " + out_error);
            return false;
        }

        // clean up stale .sldepth sidecar if embedding directly
        fs::path sidecar = base_p;
        sidecar.replace_extension(".sldepth");
        if (fs::exists(sidecar)) {
            std::error_code ec;
            fs::remove(sidecar, ec);
        }

        log(3, "Successfully embedded 3D depth map into PNG (" + std::to_string(dw) + "x" + std::to_string(dh) + ")");
    } else {
        // write .sldepth sidecar
        fs::path sidecar_p = base_p;
        sidecar_p.replace_extension(".sldepth");
        std::string sidecar_u8 = wide_to_utf8(sidecar_p.wstring().c_str());

        SidecarDepthHeader header = {};
        header.magic = kSidecarMagic;
        header.version = kSidecarVersion;
        header.encoding = 0; // R32F Deflate
        header.width = static_cast<uint32_t>(dw);
        header.height = static_cast<uint32_t>(dh);
        header.flags = is_flat ? (kSidecarFlagValid | kSidecarFlagFlat) : kSidecarFlagValid;
        header.far_plane_used = (far_plane > 0.0f) ? far_plane : 1000.0f;
        header.raw_byte_size = static_cast<uint32_t>(dw * dh * sizeof(float));
        header.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        strcpy_s(header.game_name, "ShaderLabDepth");

        if (!depth_file::write_sidecar(sidecar_u8, header, linear_floats.data(), linear_floats.size(), out_error)) {
            log(2, "Failed to write .sldepth sidecar: " + out_error);
            return false;
        }

        log(3, "Saved depth sidecar: " + wide_to_utf8(sidecar_p.filename().wstring().c_str()));
    }

    // trigger reload in host
    JobQueueManager::get().set_preview_image(base_p.wstring());
    return true;
}

bool DepthManager::trigger_ai_depth(
    const std::wstring &base_image_path,
    float far_plane,
    bool embed_png,
    const std::string &model_encoder,
    int input_size,
    float gamma,
    float near_threshold,
    float sky_threshold,
    bool edge_refine,
    bool invert,
    bool smooth_normals,
    int smooth_radius,
    float smooth_eps
) {
    if (m_ai_running.load()) {
        log(1, "AI generation already in progress...");
        return false;
    }

    if (base_image_path.empty() || !fs::exists(base_image_path)) {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        m_ai_last_error = "Active image path is empty or does not exist: " + wide_to_utf8(base_image_path.c_str());
        log(2, m_ai_last_error);
        return false;
    }

    if (m_ai_worker.joinable()) {
        m_ai_worker.join();
    }

    m_ai_running = true;
    m_ai_progress.store(-1.0f);
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        m_ai_status = "Locating Python environment and neural model...";
        m_ai_last_error = "";
    }

    log(0, "==================================================");
    log(0, "Starting Depth Anything V2 Machine Learning Depth Estimation");
    log(0, "Active Image: " + wide_to_utf8(base_image_path.c_str()));

    m_ai_worker = std::thread([this, base_image_path, far_plane, embed_png, model_encoder, input_size, gamma, near_threshold, sky_threshold, edge_refine, invert, smooth_normals, smooth_radius, smooth_eps]() {
        try {
            std::wstring py_exe = find_python_executable();
            std::wstring script = find_script_path();
            std::wstring model = find_model_path(model_encoder);

            std::error_code model_ec;
            bool model_exists = !model.empty() && fs::exists(model, model_ec) && !model_ec;
            log(0, "Python binary: " + wide_to_utf8(py_exe.c_str()));
            log(0, "Model weights: " + wide_to_utf8(model.c_str()) + " (exists: " + (model_exists ? "YES" : "NO") + ")");

            std::error_code py_ec;
            if (py_exe.empty() || !fs::exists(py_exe, py_ec) || py_ec) {
                log(1, "Python not detected in system or local folders. Auto-provisioning embedded Python...");
                std::wstring prov_py;
                if (provision_portable_python(prov_py)) {
                    py_exe = prov_py;
                    {
                        std::lock_guard<std::mutex> lock(m_cache_mutex);
                        m_cached_python_path = py_exe;
                    }
                    log(3, "Auto-provisioned portable Python successfully: " + wide_to_utf8(py_exe.c_str()));
                } else {
                    std::lock_guard<std::mutex> lock(m_status_mutex);
                    m_ai_last_error = "Could not locate or auto-download Python. Please check your internet connection or install Python 3.10+.";
                    m_ai_running = false;
                    log(2, m_ai_last_error);
                    return;
                }
            }

            std::error_code script_ec;
            if (script.empty() || !fs::exists(script, script_ec) || script_ec) {
                std::lock_guard<std::mutex> lock(m_status_mutex);
                m_ai_last_error = "Inference script not found (run_depth_anything.py). Please check that the tools folder is present.";
                m_ai_running = false;
                log(2, m_ai_last_error);
                return;
            }

            bool model_valid = !model.empty() && verify_model_file(model, model_encoder);
            if (!model_valid) {
                std::lock_guard<std::mutex> lock(m_status_mutex);
                m_ai_last_error = "Model weights for '" + model_encoder + "' not found or incomplete. Please download the model checkpoint.";
                m_ai_running = false;
                log(2, m_ai_last_error);
                return;
            }

            // auto-check and install required python packages if missing locally
            bool deps_ready = m_deps_verified.load();
            fs::path local_site_p;
            if (!script.empty()) {
                local_site_p = fs::path(script).parent_path() / "site-packages";
            }
            if (local_site_p.empty() || !fs::exists(local_site_p, py_ec)) {
                std::vector<fs::path> bdirs = get_base_search_dirs();
                for (const auto &b : bdirs) {
                    fs::path cur = b;
                    for (int d = 0; d <= 6; ++d) {
                        fs::path cand = cur / "tools" / "depth_anything_v2" / "site-packages";
                        if (fs::exists(cand, py_ec) && !py_ec) {
                            local_site_p = cand;
                            break;
                        }
                        cand = cur / "test" / "tools" / "depth_anything_v2" / "site-packages";
                        if (fs::exists(cand, py_ec) && !py_ec) {
                            local_site_p = cand;
                            break;
                        }
                        if (!cur.has_parent_path() || cur == cur.parent_path()) break;
                        cur = cur.parent_path();
                    }
                    if (!local_site_p.empty()) break;
                }
            }

            // fast-path: check if disk packages are present
            if (!deps_ready && !local_site_p.empty() && fs::exists(local_site_p, py_ec)) {
                bool has_torch = fs::exists(local_site_p / "torch", py_ec) && !py_ec;
                bool has_cv2   = fs::exists(local_site_p / "cv2", py_ec) && !py_ec;
                bool has_numpy = fs::exists(local_site_p / "numpy", py_ec) && !py_ec;
                bool has_scipy = fs::exists(local_site_p / "scipy", py_ec) && !py_ec;

                if (has_torch && has_cv2 && has_numpy && has_scipy) {
                    deps_ready = true;
                    m_deps_verified = true;
                    log(0, "Verified local AI dependencies in " + wide_to_utf8(local_site_p.wstring().c_str()) + " (torch, cv2, numpy, scipy present)");
                }
            }

            if (!deps_ready) {
                std::wstring local_site_w = local_site_p.empty() ? L"" : fs::absolute(local_site_p).wstring();
                std::wstring check_cmd = L"\"" + py_exe + L"\" -c \"import sys; "
                    + (local_site_w.empty() ? L"" : (L"sys.path.insert(0, r'" + local_site_w + L"'); "))
                    + L"import torch, cv2, numpy, scipy\"";

                STARTUPINFOW check_si = {};
                check_si.cb = sizeof(check_si);
                check_si.dwFlags = STARTF_USESHOWWINDOW;
                check_si.wShowWindow = SW_HIDE;
                PROCESS_INFORMATION check_pi = {};
                std::vector<wchar_t> check_buf(check_cmd.begin(), check_cmd.end());
                check_buf.push_back(L'\0');

                if (CreateProcessW(nullptr, check_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &check_si, &check_pi)) {
                    DWORD wait_res = WaitForSingleObject(check_pi.hProcess, 45000);
                    if (wait_res == WAIT_OBJECT_0) {
                        DWORD code = 1;
                        GetExitCodeProcess(check_pi.hProcess, &code);
                        if (code == 0) {
                            deps_ready = true;
                            m_deps_verified = true;
                            log(0, "Verified AI dependencies via Python import successfully");
                        }
                    } else if (wait_res == WAIT_TIMEOUT) {
                        log(1, "Dependency check timed out after 45s, terminating check process...");
                        TerminateProcess(check_pi.hProcess, 1);
                    }
                    CloseHandle(check_pi.hProcess);
                    CloseHandle(check_pi.hThread);
                }

                if (!deps_ready) {
                    if (local_site_w.empty()) {
                        fs::path base_dest = fs::path(py_exe).parent_path() / "tools" / "depth_anything_v2" / "site-packages";
                        fs::create_directories(base_dest, py_ec);
                        local_site_w = fs::absolute(base_dest).wstring();
                    }
                    log(1, "Missing AI dependencies (torch, cv2, numpy). Installing locally to " + wide_to_utf8(local_site_w.c_str()) + " via pip...");
                    {
                        std::lock_guard<std::mutex> lock(m_status_mutex);
                        m_ai_status = "Auto-installing AI packages locally into ShaderLab folder...";
                    }

                    std::wstring pip_cmd = L"\"" + py_exe + L"\" -m pip install --no-user --target \"" + local_site_w + L"\" torch opencv-python numpy scipy --extra-index-url https://download.pytorch.org/whl/cu126 --extra-index-url https://download.pytorch.org/whl/cu124";
                    STARTUPINFOW pip_si = {};
                    pip_si.cb = sizeof(pip_si);
                    pip_si.dwFlags = STARTF_USESHOWWINDOW;
                    pip_si.wShowWindow = SW_HIDE;
                    PROCESS_INFORMATION pip_pi = {};
                    std::vector<wchar_t> pip_buf(pip_cmd.begin(), pip_cmd.end());
                    pip_buf.push_back(L'\0');

                    if (CreateProcessW(nullptr, pip_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &pip_si, &pip_pi)) {
                        WaitForSingleObject(pip_pi.hProcess, 600000);
                        DWORD pip_exit = 1;
                        GetExitCodeProcess(pip_pi.hProcess, &pip_exit);
                        CloseHandle(pip_pi.hProcess);
                        CloseHandle(pip_pi.hThread);

                        if (pip_exit == 0) {
                            deps_ready = true;
                            m_deps_verified = true;
                            log(3, "Dependencies installed locally successfully via pip!");
                        }
                    }
                }
            }

            if (!deps_ready) {
                std::lock_guard<std::mutex> lock(m_status_mutex);
                m_ai_last_error = "Required AI dependencies (torch, cv2, numpy) could not be installed or verified.";
                m_ai_running = false;
                log(2, m_ai_last_error);
                return;
            }

            fs::path base_p = fs::absolute(base_image_path);
            fs::path sidecar_p = base_p;
            sidecar_p.replace_extension(".sldepth");
            fs::path script_dir = fs::path(script).parent_path();

            std::wstring cmdline = L"\"" + py_exe + L"\" \"" + fs::absolute(script).wstring() + L"\""
                                 + L" --input \"" + base_p.wstring() + L"\""
                                 + L" --model \"" + fs::absolute(model).wstring() + L"\""
                                 + L" --encoder " + utf8_to_wide(model_encoder.c_str())
                                 + L" --input-size " + std::to_wstring(input_size)
                                 + L" --gamma " + std::to_wstring(gamma)
                                 + L" --near-threshold " + std::to_wstring(near_threshold)
                                 + L" --sky-threshold " + std::to_wstring(sky_threshold)
                                 + (edge_refine ? L" --edge-refine" : L"")
                                 + (invert ? L" --invert" : L"")
                                 + (smooth_normals ? (L" --smooth-normals --smooth-radius " + std::to_wstring(smooth_radius) + L" --smooth-eps " + std::to_wstring(smooth_eps)) : L"")
                                 + L" --output-sidecar \"" + sidecar_p.wstring() + L"\""
                                 + L" --far-plane " + std::to_wstring(far_plane);

            log(0, "Command line: " + wide_to_utf8(cmdline.c_str()));

            {
                std::lock_guard<std::mutex> lock(m_status_mutex);
                m_ai_status = "Running PyTorch neural network inference...";
            }

            // launch subprocess with pipes
            HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
            SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
            CreatePipe(&hReadPipe, &hWritePipe, &sa, 0);
            SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

            // connect valid NUL handle to stdin so python CRT never gets an invalid handle
            HANDLE hNullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = hWritePipe;
            si.hStdError = hWritePipe;
            si.hStdInput = (hNullInput != INVALID_HANDLE_VALUE) ? hNullInput : nullptr;

            PROCESS_INFORMATION pi = {};
            std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
            cmd_buf.push_back(L'\0');

            BOOL created = CreateProcessW(
                nullptr,
                cmd_buf.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW,
                nullptr,
                script_dir.wstring().c_str(),
                &si,
                &pi
            );

            CloseHandle(hWritePipe);
            if (hNullInput != INVALID_HANDLE_VALUE) CloseHandle(hNullInput);

            if (!created) {
                CloseHandle(hReadPipe);
                std::lock_guard<std::mutex> lock(m_status_mutex);
                m_ai_last_error = "Failed to launch Python subprocess (Error " + std::to_string(GetLastError()) + ")";
                m_ai_running = false;
                log(2, m_ai_last_error);
                return;
            }

            // stream stdout/stderr
            char pipe_buf[512];
            DWORD bytes_read = 0;
            std::string line_accum;
            std::string proc_output;

            while (ReadFile(hReadPipe, pipe_buf, sizeof(pipe_buf) - 1, &bytes_read, nullptr) && bytes_read > 0) {
                pipe_buf[bytes_read] = '\0';
                proc_output += pipe_buf;
                for (DWORD i = 0; i < bytes_read; ++i) {
                    char c = pipe_buf[i];
                    if (c == '\n' || c == '\r') {
                        if (!line_accum.empty()) {
                            // parse progress markers like [PROGRESS 45%]
                            auto ppos = line_accum.find("[PROGRESS ");
                            if (ppos != std::string::npos) {
                                int pct = atoi(line_accum.c_str() + ppos + 10);
                                m_ai_progress.store(static_cast<float>(pct) / 100.0f);
                                std::lock_guard<std::mutex> lock(m_status_mutex);
                                m_ai_status = "Inference progress: " + std::to_string(pct) + "%";
                            } else {
                                log(0, "[Python] " + line_accum);
                            }
                            line_accum.clear();
                        }
                    } else {
                        line_accum += c;
                    }
                }
            }
            if (!line_accum.empty()) {
                log(0, "[Python] " + line_accum);
            }

            CloseHandle(hReadPipe);
            WaitForSingleObject(pi.hProcess, 120000); // 2 minute timeout

            DWORD exit_code = 1;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);

            log(0, "Python process exited with code: " + std::to_string(exit_code));

            std::error_code sc_ec;
            if (exit_code == 0 && fs::exists(sidecar_p, sc_ec) && !sc_ec) {
                log(0, "Sidecar file verified on disk: " + wide_to_utf8(sidecar_p.wstring().c_str()));

                // convert sidecar into slDp chunk if user requested embedded png
                std::string ext = base_p.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)::tolower(c); });

                if (embed_png && ext == ".png") {
                    log(0, "Embedding depth into PNG as slDp chunk...");
                    SidecarDepthHeader s_hdr = {};
                    std::vector<float> floats;
                    std::string read_err;
                    if (depth_file::read_sidecar(wide_to_utf8(sidecar_p.wstring().c_str()), s_hdr, floats, read_err)) {
                        DepthMapHeader c_hdr = {};
                        c_hdr.magic = kDepthMagicSLD1;
                        c_hdr.version = kDepthVersion1;
                        c_hdr.encoding = 0;
                        c_hdr.width = s_hdr.width;
                        c_hdr.height = s_hdr.height;
                        c_hdr.flags = kDepthFlagValid;
                        c_hdr.near_plane = 1.0f;
                        c_hdr.far_plane = s_hdr.far_plane_used;
                        c_hdr.raw_byte_size = s_hdr.raw_byte_size;
                        c_hdr.timestamp = s_hdr.timestamp;
                        strcpy_s(c_hdr.game_name, "DepthAnythingV2");

                        std::string inj_err;
                        if (depth_chunk::inject_sldp(wide_to_utf8(base_p.wstring().c_str()), c_hdr, floats.data(), floats.size(), inj_err)) {
                            std::error_code ec;
                            fs::remove(sidecar_p, ec);
                            log(3, "Embedded depth chunk into PNG successfully!");
                        } else {
                            log(1, "PNG chunk injection failed: " + inj_err + " (retaining .sldepth sidecar)");
                        }
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(m_status_mutex);
                    m_ai_status = "Depth generation complete!";
                    m_ai_last_error = "";
                }
                log(3, "Depth generation complete! Triggering host reload...");
                // trigger reload in host
                JobQueueManager::get().set_preview_image(base_p.wstring());
            } else {
                std::lock_guard<std::mutex> lock(m_status_mutex);
                if (proc_output.find("No module named 'torch'") != std::string::npos ||
                    proc_output.find("No module named torch") != std::string::npos) {
                    m_ai_last_error = "PyTorch is missing in your Python environment. Run: pip install torch torchvision";
                } else if (proc_output.find("No module named 'cv2'") != std::string::npos ||
                           proc_output.find("No module named cv2") != std::string::npos) {
                    m_ai_last_error = "OpenCV is missing in your Python environment. Run: pip install opencv-python";
                } else {
                    std::string trimmed_err = proc_output;
                    size_t last_nl = trimmed_err.find_last_not_of(" \r\n");
                    if (last_nl != std::string::npos) trimmed_err = trimmed_err.substr(0, last_nl + 1);
                    size_t prev_nl = trimmed_err.find_last_of("\r\n");
                    if (prev_nl != std::string::npos && prev_nl + 1 < trimmed_err.size()) {
                        trimmed_err = trimmed_err.substr(prev_nl + 1);
                    }
                    m_ai_last_error = "Inference failed (Exit " + std::to_string(exit_code) + "): " + trimmed_err;
                }
                log(2, m_ai_last_error);
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            m_ai_last_error = std::string("AI generation error: ") + e.what();
            log(2, m_ai_last_error);
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            m_ai_last_error = "AI generation error: unexpected exception";
            log(2, m_ai_last_error);
        }

        log(0, "==================================================");
        m_ai_running = false;
    });

    return true;
}

bool DepthManager::remove_depth(const std::wstring &base_image_path, std::string &out_error) {
    if (base_image_path.empty() || !fs::exists(base_image_path)) {
        out_error = "Active image not found";
        return false;
    }

    fs::path base_p = fs::absolute(base_image_path);
    fs::path sidecar_p = base_p;
    sidecar_p.replace_extension(".sldepth");

    bool removed_any = false;
    if (fs::exists(sidecar_p)) {
        std::error_code ec;
        fs::remove(sidecar_p, ec);
        removed_any = true;
    }

    // strip slDp chunk from PNG if present
    std::string ext = base_p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)::tolower(c); });
    if (ext == ".png") {
        std::ifstream in_f(base_p, std::ios::binary | std::ios::ate);
        if (in_f.is_open()) {
            size_t size = in_f.tellg();
            in_f.seekg(0, std::ios::beg);
            std::vector<uint8_t> buffer(size);
            in_f.read(reinterpret_cast<char *>(buffer.data()), size);
            in_f.close();

            if (buffer.size() > 8 && buffer[0] == 0x89 && buffer[1] == 'P' && buffer[2] == 'N' && buffer[3] == 'G') {
                std::vector<uint8_t> stripped;
                stripped.insert(stripped.end(), buffer.begin(), buffer.begin() + 8);
                size_t offset = 8;
                bool had_chunk = false;

                while (offset + 8 <= buffer.size()) {
                    uint32_t len = (buffer[offset] << 24) | (buffer[offset + 1] << 16) | (buffer[offset + 2] << 8) | buffer[offset + 3];
                    std::string chunk_type(reinterpret_cast<char *>(buffer.data() + offset + 4), 4);
                    size_t total_chunk_len = 8 + len + 4; // len + type + data + crc

                    if (offset + total_chunk_len > buffer.size()) break;

                    if (chunk_type == "slDp") {
                        had_chunk = true;
                    } else {
                        stripped.insert(stripped.end(), buffer.begin() + offset, buffer.begin() + offset + total_chunk_len);
                    }

                    offset += total_chunk_len;
                    if (chunk_type == "IEND") break;
                }

                if (had_chunk) {
                    std::ofstream out_f(base_p, std::ios::binary);
                    if (out_f.is_open()) {
                        out_f.write(reinterpret_cast<char *>(stripped.data()), stripped.size());
                        out_f.close();
                        removed_any = true;
                    } else {
                        out_error = "Failed to write modified PNG (file may be write-protected or locked)";
                        log(2, out_error);
                        return false;
                    }
                }
            }
        }
    }

    log(0, "Removed depth from active image: " + wide_to_utf8(base_p.wstring().c_str()));
    JobQueueManager::get().set_preview_image(base_p.wstring());
    return true;
}

bool DepthManager::export_depth(
    const std::wstring &base_image_path,
    const std::wstring &output_path,
    bool as_16bit_png,
    std::string &out_error
) {
    if (base_image_path.empty() || !fs::exists(base_image_path)) {
        out_error = "Active image not found";
        return false;
    }

    fs::path base_p = fs::absolute(base_image_path);
    fs::path sidecar_p = base_p;
    sidecar_p.replace_extension(".sldepth");

    std::vector<float> linear_floats;
    uint32_t dw = 0, dh = 0;
    float far_plane = 1000.0f;
    bool found = false;

    // try sidecar first
    if (fs::exists(sidecar_p)) {
        SidecarDepthHeader hdr = {};
        if (depth_file::read_sidecar(wide_to_utf8(sidecar_p.wstring().c_str()), hdr, linear_floats, out_error)) {
            dw = hdr.width;
            dh = hdr.height;
            far_plane = hdr.far_plane_used;
            found = true;
        }
    }

    // fallback: try PNG slDp chunk
    if (!found) {
        std::ifstream in_f(base_p, std::ios::binary | std::ios::ate);
        if (in_f.is_open()) {
            size_t size = in_f.tellg();
            in_f.seekg(0, std::ios::beg);
            std::vector<uint8_t> buffer(size);
            in_f.read(reinterpret_cast<char *>(buffer.data()), size);
            in_f.close();

            DepthMapHeader c_hdr = {};
            if (depth_chunk::extract_sldp(buffer.data(), buffer.size(), c_hdr, linear_floats, out_error)) {
                dw = c_hdr.width;
                dh = c_hdr.height;
                far_plane = c_hdr.far_plane;
                found = true;
            }
        }
    }

    if (!found || dw == 0 || dh == 0 || linear_floats.size() != dw * dh) {
        out_error = "No depth map found in active image to export";
        return false;
    }

    std::string out_u8 = wide_to_utf8(output_path.c_str());

    if (as_16bit_png) {
        std::vector<uint8_t> u8_buf(dw * dh);
        for (size_t i = 0; i < linear_floats.size(); ++i) {
            float v = std::clamp(linear_floats[i], 0.0f, 1.0f);
            u8_buf[i] = static_cast<uint8_t>(v * 255.0f + 0.5f);
        }
        if (!stbi_write_png(out_u8.c_str(), dw, dh, 1, u8_buf.data(), dw)) {
            out_error = "Failed to write PNG file";
            return false;
        }
    } else {
        SidecarDepthHeader hdr = {};
        hdr.magic = kSidecarMagic;
        hdr.version = kSidecarVersion;
        hdr.encoding = 0;
        hdr.width = dw;
        hdr.height = dh;
        hdr.flags = kSidecarFlagValid;
        hdr.far_plane_used = far_plane;
        hdr.raw_byte_size = static_cast<uint32_t>(dw * dh * sizeof(float));
        hdr.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        strcpy_s(hdr.game_name, "ExportedDepth");

        if (!depth_file::write_sidecar(out_u8, hdr, linear_floats.data(), linear_floats.size(), out_error)) {
            return false;
        }
    }

    log(3, "Exported depth map to: " + out_u8);
    return true;
}

