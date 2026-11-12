#include "upscale_manager.h"
#include "GUI/gui_manager.h"
#include "GUI/types.h"
#include "depth_file.h"
#include <imgui.h>
#include <urlmon.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

struct ScopedHandle {
    HANDLE handle = nullptr;
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE h) : handle(h) {}
    ~ScopedHandle() { reset(); }
    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;
    void reset(HANDLE next = nullptr) {
        if (handle && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        handle = next;
    }
    HANDLE get() const { return handle; }
    bool valid() const { return handle && handle != INVALID_HANDLE_VALUE; }
};

std::filesystem::path get_executable_dir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(buf).parent_path();
}

std::wstring to_wide_utf8(const std::string &s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), needed);
    return out;
}

bool download_file_url(const std::string &url, const std::filesystem::path &dst, std::string &out_err) {
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    std::filesystem::path tmp = dst;
    tmp += L".part";
    std::filesystem::remove(tmp, ec);

    // try system curl.exe first so TLS redirects and GitHub/CDN headers work reliably
    std::wstring cmd = L"curl.exe -fsSL --retry 2 --connect-timeout 20 -o \"" + tmp.wstring() + L"\" \"" + to_wide_utf8(url) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        ScopedHandle h_proc(pi.hProcess);
        ScopedHandle h_thread(pi.hThread);
        WaitForSingleObject(h_proc.get(), INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(h_proc.get(), &exit_code);
        if (exit_code == 0 && std::filesystem::exists(tmp, ec) && std::filesystem::file_size(tmp, ec) > 0) {
            std::filesystem::rename(tmp, dst, ec);
            if (!ec) return true;
            std::filesystem::copy_file(tmp, dst, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(tmp, ec);
            if (!ec) return true;
        }
    }

    // fallback to Win32 urlmon
    HRESULT hr = URLDownloadToFileW(nullptr, to_wide_utf8(url).c_str(), tmp.c_str(), 0, nullptr);
    if (SUCCEEDED(hr) && std::filesystem::exists(tmp, ec) && std::filesystem::file_size(tmp, ec) > 0) {
        std::filesystem::rename(tmp, dst, ec);
        if (!ec) return true;
    }
    std::filesystem::remove(tmp, ec);
    out_err = "Failed to download checkpoint from URL: " + url;
    return false;
}

} // namespace

UpscaleManager &UpscaleManager::get() {
    static UpscaleManager instance;
    return instance;
}

UpscaleManager::~UpscaleManager() {
    shutdown();
}

void UpscaleManager::initialize(const std::filesystem::path &workspace_root) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_workspace_root = workspace_root;
    if (m_status_message.empty()) {
        m_status_message = "Idle";
    }
}

void UpscaleManager::shutdown() {
    cancel();
    join_worker();
    join_download_worker();
}

void UpscaleManager::join_worker() {
    if (m_worker_thread.joinable()) {
        m_worker_thread.join();
    }
}

void UpscaleManager::join_download_worker() {
    if (m_download_thread.joinable()) {
        m_download_thread.join();
    }
}

const std::vector<UpscaleModelSpec> &UpscaleManager::get_model_catalog() {
    static const std::vector<UpscaleModelSpec> kCatalog = {
        {
            "RealESRGAN_x4plus",
            "Real-ESRGAN x4plus (Photo 4x, BSD-3)",
            "RealESRGAN_x4plus.pth",
            "BSD-3-Clause",
            "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth",
            4,
            10ull * 1024ull * 1024ull,
            false
        },
        {
            "RealESRGAN_x2plus",
            "Real-ESRGAN x2plus (Photo 2x, BSD-3)",
            "RealESRGAN_x2plus.pth",
            "BSD-3-Clause",
            "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.1/RealESRGAN_x2plus.pth",
            2,
            10ull * 1024ull * 1024ull,
            false
        },
        {
            "RealESRGAN_x4plus_anime_6B",
            "Real-ESRGAN anime_6B (Illustration 4x, BSD-3)",
            "RealESRGAN_x4plus_anime_6B.pth",
            "BSD-3-Clause",
            "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.2.4/RealESRGAN_x4plus_anime_6B.pth",
            4,
            5ull * 1024ull * 1024ull,
            false
        },
        {
            "RealPLKSR",
            "RealPLKSR WebPhoto 4x (via Spandrel, Apache-2.0)",
            "4xNomosWebPhoto_RealPLKSR.pth",
            "Apache-2.0 / CC-BY-4.0",
            "https://github.com/Phhofm/models/releases/download/4xNomosWebPhoto_RealPLKSR/4xNomosWebPhoto_RealPLKSR.pth",
            4,
            5ull * 1024ull * 1024ull,
            true
        }
    };
    return kCatalog;
}

std::filesystem::path UpscaleManager::get_script_path() const {
    const std::filesystem::path exe_dir = get_executable_dir();
    std::vector<std::filesystem::path> candidates = {
        exe_dir / "tools" / "upscale" / "run_upscale.py",
        m_workspace_root / "tools" / "upscale" / "run_upscale.py",
        std::filesystem::current_path() / "tools" / "upscale" / "run_upscale.py",
        exe_dir.parent_path() / "tools" / "upscale" / "run_upscale.py",
        exe_dir.parent_path().parent_path() / "tools" / "upscale" / "run_upscale.py"
    };
    std::error_code ec;
    for (const auto &c : candidates) {
        if (!c.empty() && std::filesystem::exists(c, ec)) {
            return c;
        }
    }
    if (!m_workspace_root.empty()) {
        return m_workspace_root / "tools" / "upscale" / "run_upscale.py";
    }
    return exe_dir / "tools" / "upscale" / "run_upscale.py";
}

std::filesystem::path UpscaleManager::get_models_dir() const {
    std::filesystem::path script = get_script_path();
    return script.parent_path() / "models";
}

std::filesystem::path UpscaleManager::get_model_path(int model_idx) const {
    const auto &cat = get_model_catalog();
    int safe_idx = std::clamp(model_idx, 0, static_cast<int>(cat.size()) - 1);
    return get_models_dir() / cat[safe_idx].filename;
}

bool UpscaleManager::is_model_available(int model_idx) const {
    const auto &cat = get_model_catalog();
    int safe_idx = std::clamp(model_idx, 0, static_cast<int>(cat.size()) - 1);
    std::filesystem::path p = get_model_path(safe_idx);
    std::error_code ec;
    return std::filesystem::exists(p, ec) && std::filesystem::file_size(p, ec) >= cat[safe_idx].expected_min_bytes;
}

std::string UpscaleManager::get_status_message() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_status_message;
}

std::string UpscaleManager::get_last_error() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_last_error;
}

void UpscaleManager::set_status(const std::string &msg, int pct) {
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_status_message = msg;
    }
    if (pct >= 0) {
        m_progress_pct.store(std::clamp(pct, 0, 100));
    }
}

void UpscaleManager::set_error(const std::string &err) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_last_error = err;
    m_status_message = "Error";
}

std::string UpscaleManager::find_python_executable() const {
    const std::filesystem::path exe_dir = get_executable_dir();
    std::vector<std::filesystem::path> candidates = {
        exe_dir / "tools" / "python" / "python.exe",
        m_workspace_root / "tools" / "python" / "python.exe",
        exe_dir / "tools" / "depth_anything_v2" / "venv" / "Scripts" / "python.exe",
        m_workspace_root / "tools" / "depth_anything_v2" / "venv" / "Scripts" / "python.exe",
        exe_dir / "tools" / "lama" / "venv" / "Scripts" / "python.exe",
        m_workspace_root / "tools" / "lama" / "venv" / "Scripts" / "python.exe",
        "C:\\Python314\\python.exe",
        "C:\\Python313\\python.exe",
        "C:\\Python312\\python.exe",
        "C:\\Python311\\python.exe"
    };

    wchar_t *local_app = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&local_app, &len, L"LOCALAPPDATA") == 0 && local_app) {
        std::filesystem::path la(local_app);
        free(local_app);
        for (const wchar_t *ver : { L"Python314", L"Python313", L"Python312", L"Python311" }) {
            candidates.push_back(la / "Programs" / "Python" / ver / "python.exe");
        }
    }

    std::error_code ec;
    for (const auto &c : candidates) {
        if (!c.empty() && std::filesystem::exists(c, ec)) {
            return gui::to_utf8(c.wstring());
        }
    }

    wchar_t path_buf[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, path_buf, nullptr) > 0) {
        return gui::to_utf8(path_buf);
    }
    return {};
}

void UpscaleManager::download_model_async(int model_idx) {
    if (m_downloading.load() || m_running.load()) return;
    join_download_worker();

    const auto &cat = get_model_catalog();
    int safe_idx = std::clamp(model_idx, 0, static_cast<int>(cat.size()) - 1);
    const UpscaleModelSpec spec = cat[safe_idx];
    const std::filesystem::path dst = get_model_path(safe_idx);

    m_downloading.store(true);
    m_progress_pct.store(2);
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_last_error.clear();
        m_status_message = std::string("Downloading ") + spec.filename + "...";
    }

    m_download_thread = std::thread([this, spec, dst]() {
        std::string err;
        bool ok = download_file_url(spec.url, dst, err);
        std::error_code ec;
        if (ok && (!std::filesystem::exists(dst, ec) || std::filesystem::file_size(dst, ec) < spec.expected_min_bytes)) {
            std::filesystem::remove(dst, ec);
            err = "Downloaded model file was smaller than expected.";
            ok = false;
        }
        if (ok) {
            set_status("Model ready", 100);
        } else {
            set_error(err);
        }
        m_downloading.store(false);
    });
}

void UpscaleManager::compute_target_dimensions(uint32_t src_w, uint32_t src_h, uint32_t &out_w, uint32_t &out_h) const {
    if (src_w == 0 || src_h == 0) {
        out_w = 0;
        out_h = 0;
        return;
    }
    if (m_settings.scale_mode == 0) {
        out_w = src_w * 2;
        out_h = src_h * 2;
    } else if (m_settings.scale_mode == 1) {
        out_w = src_w * 4;
        out_h = src_h * 4;
    } else {
        out_w = static_cast<uint32_t>(std::max(16, m_settings.custom_width));
        out_h = static_cast<uint32_t>(std::max(16, m_settings.custom_height));
    }
}

bool UpscaleManager::upscale_depth_sidecar_bilinear(
    const std::filesystem::path &src_depth,
    const std::filesystem::path &dst_depth,
    uint32_t target_w,
    uint32_t target_h,
    std::string &out_error)
{
    if (target_w == 0 || target_h == 0) {
        out_error = "Invalid target dimensions for depth upscaling";
        return false;
    }

    SidecarDepthHeader hdr{};
    std::vector<float> src_vals;
    if (!depth_file::read_sidecar(gui::to_utf8(src_depth.wstring()), hdr, src_vals, out_error)) {
        return false;
    }
    if (hdr.width == 0 || hdr.height == 0 || src_vals.size() < static_cast<size_t>(hdr.width) * hdr.height) {
        out_error = "Source depth sidecar dimensions invalid";
        return false;
    }

    std::vector<float> dst_vals(static_cast<size_t>(target_w) * target_h, 0.0f);
    const float sx = (target_w > 1) ? static_cast<float>(hdr.width - 1) / static_cast<float>(target_w - 1) : 0.0f;
    const float sy = (target_h > 1) ? static_cast<float>(hdr.height - 1) / static_cast<float>(target_h - 1) : 0.0f;

    for (uint32_t y = 0; y < target_h; ++y) {
        float fy = y * sy;
        uint32_t y0 = static_cast<uint32_t>(fy);
        uint32_t y1 = std::min(y0 + 1, hdr.height - 1);
        float wy = fy - static_cast<float>(y0);

        for (uint32_t x = 0; x < target_w; ++x) {
            float fx = x * sx;
            uint32_t x0 = static_cast<uint32_t>(fx);
            uint32_t x1 = std::min(x0 + 1, hdr.width - 1);
            float wx = fx - static_cast<float>(x0);

            float d00 = src_vals[static_cast<size_t>(y0) * hdr.width + x0];
            float d10 = src_vals[static_cast<size_t>(y0) * hdr.width + x1];
            float d01 = src_vals[static_cast<size_t>(y1) * hdr.width + x0];
            float d11 = src_vals[static_cast<size_t>(y1) * hdr.width + x1];

            float top = d00 + (d10 - d00) * wx;
            float bot = d01 + (d11 - d01) * wx;
            dst_vals[static_cast<size_t>(y) * target_w + x] = std::clamp(top + (bot - top) * wy, 0.0f, 1.0f);
        }
    }

    SidecarDepthHeader out_hdr = hdr;
    out_hdr.width = target_w;
    out_hdr.height = target_h;
    return depth_file::write_sidecar(
        gui::to_utf8(dst_depth.wstring()),
        out_hdr,
        dst_vals.data(),
        dst_vals.size(),
        out_error
    );
}

bool UpscaleManager::start_upscale(
    const std::filesystem::path &input_image_path,
    uint32_t src_w,
    uint32_t src_h,
    const std::filesystem::path &input_depth_path)
{
    if (m_running.load() || m_downloading.load()) {
        set_error("Upscaler is already busy.");
        return false;
    }

    const std::string py_exe = find_python_executable();
    if (py_exe.empty()) {
        set_error("Python 3 runtime not found. Install Python 3.11+ or use Depth tab setup first.");
        return false;
    }

    const std::filesystem::path script_path = get_script_path();
    std::error_code ec;
    if (!std::filesystem::exists(script_path, ec)) {
        set_error("Missing script: " + gui::to_utf8(script_path.wstring()));
        return false;
    }

    join_worker();

    uint32_t target_w = 0;
    uint32_t target_h = 0;
    compute_target_dimensions(src_w, src_h, target_w, target_h);
    if (target_w == 0 || target_h == 0) {
        set_error("Invalid image dimensions for upscaling.");
        return false;
    }

    const auto &cat = get_model_catalog();
    int safe_model_idx = std::clamp(m_settings.model_idx, 0, static_cast<int>(cat.size()) - 1);
    const UpscaleModelSpec spec = cat[safe_model_idx];

    int tile_px = 512;
    if (m_settings.tile_mode == 1) tile_px = 256;
    else if (m_settings.tile_mode == 2) tile_px = 768;
    else if (m_settings.tile_mode == 3) tile_px = 0;

    const UpscaleSettings job_settings = m_settings;
    const std::filesystem::path models_dir = get_models_dir();
    std::filesystem::path out_image_path = input_image_path;
    out_image_path.replace_filename(input_image_path.stem().wstring() + L"_upscaled.png");

    std::filesystem::path out_depth_path;
    const bool do_depth = job_settings.upscale_depth && !input_depth_path.empty() && std::filesystem::exists(input_depth_path, ec);
    if (do_depth) {
        out_depth_path = out_image_path;
        out_depth_path.replace_extension(L".sldepth");
    }

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_last_error.clear();
        m_status_message = "Starting AI upscaler...";
        m_has_completed = false;
        m_completed_ok = false;
        m_completed_image_path.clear();
        m_completed_depth_path.clear();
        m_completed_error.clear();
    }
    m_cancel_requested.store(false);
    m_progress_pct.store(1);
    m_running.store(true);

    m_worker_thread = std::thread([=]() {
        // Optional on-demand install of lightweight MIT spandrel if RealPLKSR selected and not yet installed
        if (spec.requires_spandrel) {
            std::filesystem::path local_sp = script_path.parent_path() / "site-packages";
            std::wstring check_cmd = L"\"" + to_wide_utf8(py_exe) + L"\" -c \"import sys; sys.path.insert(0, r'" +
                local_sp.wstring() + L"'); import spandrel\"";
            STARTUPINFOW csi{};
            csi.cb = sizeof(csi);
            csi.dwFlags = STARTF_USESHOWWINDOW;
            csi.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION cpi{};
            DWORD test_exit = 1;
            if (CreateProcessW(nullptr, check_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &csi, &cpi)) {
                WaitForSingleObject(cpi.hProcess, 15000);
                GetExitCodeProcess(cpi.hProcess, &test_exit);
                CloseHandle(cpi.hProcess);
                CloseHandle(cpi.hThread);
            }
            if (test_exit != 0) {
                set_status("Installing Spandrel (MIT)...", 3);
                std::wstring pip_cmd = L"\"" + to_wide_utf8(py_exe) + L"\" -m pip install --no-deps --target \"" +
                    local_sp.wstring() + L"\" spandrel typing-extensions safetensors einops";
                STARTUPINFOW psi{};
                psi.cb = sizeof(psi);
                psi.dwFlags = STARTF_USESHOWWINDOW;
                psi.wShowWindow = SW_HIDE;
                PROCESS_INFORMATION ppi{};
                if (CreateProcessW(nullptr, pip_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &psi, &ppi)) {
                    WaitForSingleObject(ppi.hProcess, INFINITE);
                    CloseHandle(ppi.hProcess);
                    CloseHandle(ppi.hThread);
                }
            }
        }

        std::wstring cmd;
        cmd.reserve(1024);
        cmd += L"\"";
        cmd += to_wide_utf8(py_exe);
        cmd += L"\" \"";
        cmd += script_path.wstring();
        cmd += L"\" --input \"";
        cmd += input_image_path.wstring();
        cmd += L"\" --output \"";
        cmd += out_image_path.wstring();
        cmd += L"\" --model \"";
        cmd += to_wide_utf8(spec.key);
        cmd += L"\" --model-dir \"";
        cmd += models_dir.wstring();
        cmd += L"\" --target-width ";
        cmd += std::to_wstring(target_w);
        cmd += L" --target-height ";
        cmd += std::to_wstring(target_h);
        cmd += L" --tile ";
        cmd += std::to_wstring(tile_px);
        cmd += L" --tile-pad 32 --blend ";
        wchar_t blend_buf[32] = {};
        swprintf_s(blend_buf, L"%.4f", std::clamp(job_settings.blend_strength, 0.0f, 1.0f));
        cmd += blend_buf;
        cmd += L" --auto-download ";
        cmd += job_settings.use_fp16 ? L"--fp16 " : L"--no-fp16 ";
        cmd += job_settings.upscale_alpha ? L"--upscale-alpha" : L"--no-upscale-alpha";

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE read_pipe_raw = nullptr;
        HANDLE write_pipe_raw = nullptr;
        if (!CreatePipe(&read_pipe_raw, &write_pipe_raw, &sa, 0)) {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_last_error = "Failed to create upscaler stdout pipe.";
            m_status_message = "Error";
            m_completed_error = m_last_error;
            m_has_completed = true;
            m_completed_ok = false;
            m_running.store(false);
            return;
        }
        ScopedHandle read_pipe(read_pipe_raw);
        ScopedHandle write_pipe(write_pipe_raw);
        SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdOutput = write_pipe.get();
        si.hStdError = write_pipe.get();
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_last_error = "Failed to launch upscaler Python process.";
            m_status_message = "Error";
            m_completed_error = m_last_error;
            m_has_completed = true;
            m_completed_ok = false;
            m_running.store(false);
            return;
        }

        // close parent write end so ReadFile gets EOF cleanly on process exit
        write_pipe.reset();

        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_active_process = pi.hProcess;
        }
        ScopedHandle h_thread(pi.hThread);

        std::string tail_logs;
        std::string line_acc;
        char read_buf[1024];
        DWORD bytes_read = 0;
        while (ReadFile(read_pipe.get(), read_buf, sizeof(read_buf) - 1, &bytes_read, nullptr) && bytes_read > 0) {
            read_buf[bytes_read] = '\0';
            line_acc.append(read_buf, bytes_read);

            size_t pos = 0;
            while ((pos = line_acc.find('\n')) != std::string::npos) {
                std::string line = line_acc.substr(0, pos);
                line_acc.erase(0, pos + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                    line.pop_back();
                }
                if (line.empty()) continue;

                const std::string prefix = "[PROGRESS] ";
                if (line.rfind(prefix, 0) == 0) {
                    size_t colon = line.find(':', prefix.size());
                    if (colon != std::string::npos) {
                        int pct = atoi(line.substr(prefix.size(), colon - prefix.size()).c_str());
                        std::string msg = line.substr(colon + 1);
                        size_t first_ns = msg.find_first_not_of(" \t");
                        if (first_ns != std::string::npos) msg.erase(0, first_ns);
                        set_status(msg, do_depth ? std::min(96, pct) : pct);
                    }
                } else {
                    if (tail_logs.size() > 2048) {
                        tail_logs.erase(0, tail_logs.size() - 1024);
                    }
                    tail_logs += line + "\n";
                }
            }
        }

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_active_process = nullptr;
        }
        CloseHandle(pi.hProcess);

        std::error_code inner_ec;
        bool ok = (exit_code == 0) && !m_cancel_requested.load() && std::filesystem::exists(out_image_path, inner_ec);
        std::string err_msg;
        std::filesystem::path final_depth_path;

        if (!ok) {
            if (m_cancel_requested.load()) {
                err_msg = "Upscale cancelled.";
            } else if (!tail_logs.empty()) {
                err_msg = tail_logs;
            } else {
                err_msg = "Upscaler exited with code " + std::to_string(exit_code);
            }
        } else if (do_depth) {
            set_status("Upscaling companion .sldepth...", 98);
            std::string depth_err;
            if (upscale_depth_sidecar_bilinear(input_depth_path, out_depth_path, target_w, target_h, depth_err)) {
                final_depth_path = out_depth_path;
            }
            // keep upscaled image even if sidecar failed, but attach note if desired
        }

        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_has_completed = true;
            m_completed_ok = ok;
            if (ok) {
                m_completed_image_path = out_image_path;
                m_completed_depth_path = final_depth_path;
                m_completed_error.clear();
                m_last_error.clear();
                m_status_message = "Upscale complete";
                m_progress_pct.store(100);
            } else {
                m_completed_image_path.clear();
                m_completed_depth_path.clear();
                m_completed_error = err_msg;
                m_last_error = err_msg;
                m_status_message = m_cancel_requested.load() ? "Cancelled" : "Error";
            }
        }
        m_running.store(false);
    });

    return true;
}

bool UpscaleManager::consume_completed_result(
    std::filesystem::path &out_image_path,
    std::filesystem::path &out_depth_path,
    std::string &out_error)
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    if (!m_has_completed) return false;
    m_has_completed = false;
    out_image_path = m_completed_image_path;
    out_depth_path = m_completed_depth_path;
    out_error = m_completed_ok ? std::string() : m_completed_error;
    return true;
}

void UpscaleManager::cancel() {
    m_cancel_requested.store(true);
    std::lock_guard<std::mutex> lock(m_state_mutex);
    if (m_active_process && m_active_process != INVALID_HANDLE_VALUE) {
        TerminateProcess(m_active_process, 1);
    }
}

bool UpscaleManager::pop_upscale_request() {
    bool req = m_ui_req_run;
    m_ui_req_run = false;
    return req;
}

void UpscaleManager::render_upscale_tab(
    gui::GuiManager &gui,
    bool has_image,
    uint32_t img_w,
    uint32_t img_h,
    bool has_depth,
    bool include_run_button)
{
    if (has_image && img_w > 0 && img_h > 0) {
        if (m_settings.last_src_w != img_w || m_settings.last_src_h != img_h) {
            m_settings.last_src_w = img_w;
            m_settings.last_src_h = img_h;
            if (m_settings.scale_mode == 2 && (m_settings.custom_width <= 0 || m_settings.custom_height <= 0)) {
                m_settings.custom_width = static_cast<int>(img_w * 2);
                m_settings.custom_height = static_cast<int>(img_h * 2);
            }
        }
    }

    const auto &catalog = get_model_catalog();
    const bool busy = m_running.load() || m_downloading.load();

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f), "AI Super-Resolution");
    ImGui::TextDisabled("Permissive local PyTorch models with tiled FP16 VRAM protection and alpha preservation.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    if (busy) ImGui::BeginDisabled();

    // Model selector
    ImGui::TextUnformatted("Model");
    ImGui::SetNextItemWidth(-1.0f);
    const char *preview_model = catalog[std::clamp(m_settings.model_idx, 0, static_cast<int>(catalog.size()) - 1)].display_name;
    if (ImGui::BeginCombo("##upscale_model", preview_model)) {
        for (int i = 0; i < static_cast<int>(catalog.size()); ++i) {
            bool selected = (m_settings.model_idx == i);
            if (ImGui::Selectable(catalog[i].display_name, selected)) {
                m_settings.model_idx = i;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    gui.register_dom_last_item("upscale_combo_model", preview_model, "combo", false, !busy);

    const bool model_ready = is_model_available(m_settings.model_idx);
    if (!model_ready) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.32f, 1.0f),
            "Checkpoint not cached locally; it will download automatically when upscaling starts.");
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("Download Model Now")) {
            download_model_async(m_settings.model_idx);
        }
    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "Checkpoint ready (%s)",
            catalog[std::clamp(m_settings.model_idx, 0, static_cast<int>(catalog.size()) - 1)].license);
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();

    // Output scale selector
    static constexpr const char *kScaleLabels[] = { "2x", "4x", "Custom" };
    m_settings.scale_mode = std::clamp(m_settings.scale_mode, 0, 2);
    ImGui::TextUnformatted("Output Scale");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##upscale_scale", kScaleLabels[m_settings.scale_mode])) {
        for (int i = 0; i < 3; ++i) {
            bool sel = (m_settings.scale_mode == i);
            if (ImGui::Selectable(kScaleLabels[i], sel)) {
                m_settings.scale_mode = i;
                if (i == 2 && has_image && img_w > 0 && img_h > 0) {
                    m_settings.custom_width = static_cast<int>(img_w * 2);
                    m_settings.custom_height = static_cast<int>(img_h * 2);
                }
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    gui.register_dom_last_item("upscale_combo_scale", kScaleLabels[m_settings.scale_mode], "combo", false, !busy);

    if (m_settings.scale_mode == 2) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Width");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputInt("##upscale_custom_w", &m_settings.custom_width, 64, 256)) {
            m_settings.custom_width = std::clamp(m_settings.custom_width, 16, 16384);
            if (m_settings.lock_aspect && img_w > 0 && img_h > 0) {
                double aspect = static_cast<double>(img_h) / static_cast<double>(img_w);
                m_settings.custom_height = std::max(16, static_cast<int>(std::llround(m_settings.custom_width * aspect)));
            }
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Height");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputInt("##upscale_custom_h", &m_settings.custom_height, 64, 256)) {
            m_settings.custom_height = std::clamp(m_settings.custom_height, 16, 16384);
            if (m_settings.lock_aspect && img_w > 0 && img_h > 0) {
                double aspect = static_cast<double>(img_w) / static_cast<double>(img_h);
                m_settings.custom_width = std::max(16, static_cast<int>(std::llround(m_settings.custom_height * aspect)));
            }
        }
        ImGui::Checkbox("Lock Aspect Ratio", &m_settings.lock_aspect);
    }

    if (has_image && img_w > 0 && img_h > 0) {
        uint32_t out_w = 0, out_h = 0;
        compute_target_dimensions(img_w, img_h, out_w, out_h);
        ImGui::TextDisabled("Target: %u x %u px", out_w, out_h);
    }

    ImGui::Spacing();

    // VRAM Tile Size
    static constexpr const char *kTileLabels[] = {
        "512 px (Auto)",
        "256 px (Low VRAM)",
        "768 px (Fast)",
        "No Tiling"
    };
    m_settings.tile_mode = std::clamp(m_settings.tile_mode, 0, 3);
    ImGui::TextUnformatted("VRAM Tile Size");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##upscale_tile", kTileLabels[m_settings.tile_mode])) {
        for (int i = 0; i < 4; ++i) {
            bool sel = (m_settings.tile_mode == i);
            if (ImGui::Selectable(kTileLabels[i], sel)) {
                m_settings.tile_mode = i;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    gui.register_dom_last_item("upscale_combo_tile", kTileLabels[m_settings.tile_mode], "combo", false, !busy);

    ImGui::Spacing();

    // Denoise / Blend strength (0% bicubic .. 100% full AI)
    float blend_pct = m_settings.blend_strength * 100.0f;
    ImGui::TextUnformatted("Detail Blend Strength");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##upscale_blend", &blend_pct, 0.0f, 100.0f, "%.0f%%")) {
        m_settings.blend_strength = std::clamp(blend_pct / 100.0f, 0.0f, 1.0f);
    }
    gui.register_dom_last_item("upscale_slider_blend", "Detail Blend Strength", "slider", false, !busy);

    ImGui::Spacing();

    // Companion depth map upscaling toggle
    ImGui::Checkbox("Upscale Depth Map (.sldepth)", &m_settings.upscale_depth);
    gui.register_dom_last_item("upscale_chk_depth", "Upscale Depth Map (.sldepth)", "checkbox", m_settings.upscale_depth, !busy);
    if (m_settings.upscale_depth && !has_depth) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("No active .sldepth map attached to current image.");
        ImGui::PopTextWrapPos();
    }

    ImGui::Checkbox("Upscale & Preserve Alpha", &m_settings.upscale_alpha);
    ImGui::Checkbox("CUDA FP16 Half Precision", &m_settings.use_fp16);

    if (busy) ImGui::EndDisabled();

    ImGui::Spacing();
    if (busy) {
        int pct = get_progress_pct();
        std::string status = get_status_message();
        char ov[128] = {};
        snprintf(ov, sizeof(ov), "%d%% - %s", pct, status.c_str());
        ImGui::ProgressBar(static_cast<float>(pct) / 100.0f, ImVec2(-1.0f, 18.0f), ov);
        if (m_running.load() && ImGui::SmallButton("Cancel")) {
            cancel();
        }
    } else {
        std::string err = get_last_error();
        if (!err.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", err.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    if (include_run_button) {
        ImGui::Spacing();
        render_upscale_bottom_button(gui, has_image);
    }
}

void UpscaleManager::render_upscale_bottom_button(gui::GuiManager &gui, bool has_image) {
    const bool busy = m_running.load() || m_downloading.load();
    const bool disabled = !has_image || busy;
    if (disabled) ImGui::BeginDisabled();

    const char *label = busy ? "Upscaling..." : "Run AI Upscale";
    if (ImGui::Button(label, ImVec2(-1.0f, 24.0f))) {
        m_ui_req_run = true;
    }
    gui.register_dom_last_item("upscale_btn_run", label, "button", false, !disabled);

    if (disabled) ImGui::EndDisabled();
}
