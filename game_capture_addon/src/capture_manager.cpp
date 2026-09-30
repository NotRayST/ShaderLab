#include "capture_manager.h"
#include "../../common/depth_file.h"
#include "../../common/depth_chunk.h"
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <filesystem>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <windows.h>
#include <shellapi.h>

namespace fs = std::filesystem;

CaptureManager& CaptureManager::get() {
    static CaptureManager instance;
    return instance;
}

void CaptureManager::init() {
}

void CaptureManager::log(const std::string& level, const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::stringstream ss;
    struct tm buf;
    localtime_s(&buf, &in_time_t);
    ss << std::put_time(&buf, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();

    std::string line = "[" + ss.str() + "] [" + level + "] " + msg;

    std::lock_guard<std::mutex> lock(m_log_mutex);
    if (!m_log_file.is_open()) {
        std::string log_path = get_log_file_path();
        m_log_file.open(log_path, std::ios::out | std::ios::app);
    }
    if (m_log_file.is_open()) {
        m_log_file << line << std::endl;
        m_log_file.flush();
    }
    OutputDebugStringA((line + "\n").c_str());
}

std::string CaptureManager::get_log_file_path() const {
    char exe_path[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    return (fs::path(exe_path).parent_path() / "ShaderLabCapture.log").string();
}

static constexpr const char* kShaderLabCaptureFxContent = R"(/*
 * ShaderLabCapture.fx - Linearized depth exporter companion technique for ShaderLab
 */

#include "ReShade.fxh"

texture ShaderLab_DepthExportTex < pooled = false; > {
    Width = BUFFER_WIDTH;
    Height = BUFFER_HEIGHT;
    Format = R32F;
};

void PS_ShaderLabDepthExport(float4 vpos : SV_Position, float2 texcoord : TEXCOORD, out float out_depth : SV_Target)
{
    out_depth = ReShade::GetLinearizedDepth(texcoord);
}

technique ShaderLabDepthCapture <
    enabled = true;
    hidden = true;
    ui_label = "ShaderLab Depth Capture";
    ui_tooltip = "Exports perspective-linearized 32-bit hardware depth for ShaderLab PNG embedding";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_ShaderLabDepthExport;
        RenderTarget = ShaderLab_DepthExportTex;
    }
}
)";

void CaptureManager::ensure_shader_file(reshade::api::effect_runtime* runtime) {
    char exe_path[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    fs::path base_dir = fs::path(exe_path).parent_path();

    std::vector<fs::path> candidate_dirs = {
        base_dir / "reshade-shaders" / "Shaders",
        base_dir / "Shaders",
        base_dir
    };

    for (const auto& dir : candidate_dirs) {
        if (fs::exists(dir)) {
            fs::path target_file = dir / "ShaderLabCapture.fx";
            bool need_write = true;
            if (fs::exists(target_file)) {
                std::ifstream ifs(target_file);
                std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                if (content.find("ShaderLabDepthCapture") != std::string::npos && content.find("hidden = true") != std::string::npos) {
                    need_write = false;
                }
            }
            if (need_write) {
                std::ofstream ofs(target_file);
                if (ofs.is_open()) {
                    ofs << kShaderLabCaptureFxContent;
                    ofs.close();
                    log("INFO", "Auto-deployed ShaderLabCapture.fx to: " + target_file.string());
                    if (runtime) {
                        reshade::api::effect_technique tech = runtime->find_technique(nullptr, "ShaderLabDepthCapture");
                        if (tech.handle != 0) {
                            runtime->set_technique_state(tech, true);
                        }
                    }
                }
            }
            break;
        }
    }
}

void CaptureManager::on_reshade_reloaded_effects(reshade::api::effect_runtime* runtime) {
    m_depth_resource = { 0 };
    m_technique_found = false;
    log("INFO", "Effects reloaded - resetting harvested depth texture handles");

    if (runtime) {
        ensure_shader_file(runtime);
        reshade::api::effect_technique tech = runtime->find_technique(nullptr, "ShaderLabDepthCapture");
        if (tech.handle != 0) {
            runtime->set_technique_state(tech, true);
        }
    }
}

void CaptureManager::on_reshade_begin_effects(
    reshade::api::effect_runtime* runtime,
    reshade::api::command_list* /*cmd_list*/,
    reshade::api::resource_view /*rtv*/,
    reshade::api::resource_view /*rtv_srgb*/)
{
    if (!runtime) return;
    if (!m_enabled) {
        m_depth_resource = { 0 };
        m_technique_found = false;
        return;
    }

    reshade::api::device* device = runtime->get_device();
    if (!device) return;

    reshade::api::effect_texture_variable tex_var = runtime->find_texture_variable(nullptr, "ShaderLab_DepthExportTex");
    if (tex_var.handle != 0) {
        reshade::api::resource_view srv = {}, srv_srgb = {};
        runtime->get_texture_binding(tex_var, &srv, &srv_srgb);
        if (srv.handle != 0) {
            reshade::api::resource res = device->get_resource_from_view(srv);
            if (res.handle != 0) {
                m_depth_resource = res;
                m_depth_desc = device->get_resource_desc(res);
                m_technique_found = true;
                return;
            }
        }
    }

    m_technique_found = false;
}

bool CaptureManager::embed_depth_in_png(reshade::api::effect_runtime* runtime, const std::string& png_path) {
    if (!runtime) return false;

    reshade::api::device* device = runtime->get_device();
    reshade::api::command_queue* queue = runtime->get_command_queue();
    if (!device || !queue) return false;

    if (m_depth_resource.handle == 0) {
        reshade::api::effect_texture_variable tex_var = runtime->find_texture_variable(nullptr, "ShaderLab_DepthExportTex");
        if (tex_var.handle != 0) {
            reshade::api::resource_view srv = {}, srv_srgb = {};
            runtime->get_texture_binding(tex_var, &srv, &srv_srgb);
            if (srv.handle != 0) {
                reshade::api::resource res = device->get_resource_from_view(srv);
                if (res.handle != 0) {
                    m_depth_resource = res;
                    m_depth_desc = device->get_resource_desc(res);
                    m_technique_found = true;
                }
            }
        }
    }

    if (!m_technique_found || m_depth_resource.handle == 0) {
        log("WARN", "ShaderLabCapture.fx is NOT active or texture was missing. Depth embedding skipped.");
        m_last_capture_status = "Screenshot saved, but ShaderLabCapture.fx was not active!";
        return false;
    }

    uint32_t d_w = m_depth_desc.texture.width;
    uint32_t d_h = m_depth_desc.texture.height;

    reshade::api::resource staging = {};
    if (!device->create_resource(
            reshade::api::resource_desc(
                d_w, d_h, 1, 1,
                reshade::api::format::r32_float, 1,
                reshade::api::memory_heap::readback,
                reshade::api::resource_usage::copy_dest),
            nullptr,
            reshade::api::resource_usage::copy_dest,
            &staging))
    {
        log("ERROR", "Failed to create depth staging readback resource");
        m_last_capture_status = "Failed: staging creation error";
        return false;
    }

    reshade::api::command_list* cmd_list = queue->get_immediate_command_list();
    if (cmd_list) {
        cmd_list->barrier(m_depth_resource, reshade::api::resource_usage::shader_resource, reshade::api::resource_usage::copy_source);
        cmd_list->copy_texture_region(m_depth_resource, 0, nullptr, staging, 0, nullptr);
        cmd_list->barrier(m_depth_resource, reshade::api::resource_usage::copy_source, reshade::api::resource_usage::shader_resource);
        
        reshade::api::fence copy_fence = {};
        if (!device->create_fence(0, reshade::api::fence_flags::none, &copy_fence) ||
            !queue->signal(copy_fence, 1) ||
            !device->wait(copy_fence, 1))
        {
            queue->wait_idle();
        }
        device->destroy_fence(copy_fence);
    }

    reshade::api::subresource_data mapped = {};
    if (device->map_texture_region(staging, 0, nullptr, reshade::api::map_access::read_only, &mapped)) {
        std::vector<float> linear_floats(d_w * d_h);
        const uint8_t* src_row = static_cast<const uint8_t*>(mapped.data);

        for (uint32_t y = 0; y < d_h; ++y) {
            const float* src = reinterpret_cast<const float*>(src_row + y * mapped.row_pitch);
            float* dst = linear_floats.data() + y * d_w;
            std::memcpy(dst, src, d_w * sizeof(float));
        }

        device->unmap_texture_region(staging, 0);
        device->destroy_resource(staging);

        bool is_flat = false;
        float min_val = 1.0f, max_val = 0.0f;
        depth_file::sanitize_and_analyze(linear_floats.data(), linear_floats.size(), is_flat, min_val, max_val);

        float far_plane = 1000.0f;
        char far_str[32] = "";
        if (runtime->get_preprocessor_definition("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", far_str)) {
            try { far_plane = std::stof(far_str); } catch (...) { far_plane = 1000.0f; }
        }

        uint32_t raw_size = 0;
        if (m_depth_bit_depth == 1) {
            raw_size = d_w * d_h * sizeof(uint16_t);
        } else if (m_depth_bit_depth == 2) {
            raw_size = d_w * d_h * sizeof(uint8_t);
        } else if (m_depth_bit_depth == 3) {
            raw_size = (d_w * d_h + 1) / 2;
        } else {
            raw_size = d_w * d_h * sizeof(float);
        }

        auto now = std::chrono::system_clock::now();
        DepthMapHeader header = {};
        header.magic = kDepthMagicSLD1;
        header.version = kDepthVersion1;
        header.encoding = static_cast<uint16_t>(m_depth_bit_depth);
        header.width = d_w;
        header.height = d_h;
        header.flags = is_flat ? 0 : kDepthFlagValid;
        header.near_plane = 1.0f;
        header.far_plane = far_plane;
        header.raw_byte_size = raw_size;
        header.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        strcpy_s(header.game_name, "GameCapture");

        std::string err;
        bool inject_ok = depth_chunk::inject_sldp(png_path, header, linear_floats.data(), linear_floats.size(), err);

        if (inject_ok) {
            log("INFO", "Successfully embedded 3D depth chunk into PNG: " + png_path +
                        " (" + std::to_string(d_w) + "x" + std::to_string(d_h) +
                        ", min=" + std::to_string(min_val) + ", max=" + std::to_string(max_val) +
                        (is_flat ? ", FLAT/UNIFORM DETECTED)" : ")"));

            m_last_capture_w = d_w;
            m_last_capture_h = d_h;
            m_last_min_depth = min_val;
            m_last_max_depth = max_val;
            m_last_is_flat = is_flat;
            m_last_saved_png_path = png_path;
            m_last_capture_status = is_flat ? "Captured (Warning: Depth was flat)" : "Captured OK with Embedded 3D Depth!";
            m_last_capture_success = true;
            return true;
        } else {
            log("ERROR", "Failed to inject slDp chunk into PNG: " + err);
            m_last_capture_status = "Failed to inject depth chunk: " + err;
            return false;
        }
    } else {
        device->destroy_resource(staging);
        log("ERROR", "Failed to map depth staging buffer");
        m_last_capture_status = "Failed: map staging buffer";
        return false;
    }
}

bool CaptureManager::save_depth_sidecar(reshade::api::effect_runtime* runtime, const std::string& sidecar_path) {
    if (!runtime) return false;

    reshade::api::device* device = runtime->get_device();
    reshade::api::command_queue* queue = runtime->get_command_queue();
    if (!device || !queue) return false;

    if (m_depth_resource.handle == 0) {
        reshade::api::effect_texture_variable tex_var = runtime->find_texture_variable(nullptr, "ShaderLab_DepthExportTex");
        if (tex_var.handle != 0) {
            reshade::api::resource_view srv = {}, srv_srgb = {};
            runtime->get_texture_binding(tex_var, &srv, &srv_srgb);
            if (srv.handle != 0) {
                reshade::api::resource res = device->get_resource_from_view(srv);
                if (res.handle != 0) {
                    m_depth_resource = res;
                    m_depth_desc = device->get_resource_desc(res);
                    m_technique_found = true;
                }
            }
        }
    }

    if (!m_technique_found || m_depth_resource.handle == 0) {
        log("WARN", "ShaderLabCapture.fx is NOT active or texture was missing. Depth sidecar skipped.");
        m_last_capture_status = "Screenshot saved, but ShaderLabCapture.fx was not active!";
        return false;
    }

    uint32_t d_w = m_depth_desc.texture.width;
    uint32_t d_h = m_depth_desc.texture.height;

    reshade::api::resource staging = {};
    if (!device->create_resource(
            reshade::api::resource_desc(
                d_w, d_h, 1, 1,
                reshade::api::format::r32_float, 1,
                reshade::api::memory_heap::readback,
                reshade::api::resource_usage::copy_dest),
            nullptr,
            reshade::api::resource_usage::copy_dest,
            &staging))
    {
        log("ERROR", "Failed to create depth staging readback resource");
        m_last_capture_status = "Failed: staging creation error";
        return false;
    }

    reshade::api::command_list* cmd_list = queue->get_immediate_command_list();
    if (cmd_list) {
        cmd_list->barrier(m_depth_resource, reshade::api::resource_usage::shader_resource, reshade::api::resource_usage::copy_source);
        cmd_list->copy_texture_region(m_depth_resource, 0, nullptr, staging, 0, nullptr);
        cmd_list->barrier(m_depth_resource, reshade::api::resource_usage::copy_source, reshade::api::resource_usage::shader_resource);
        
        reshade::api::fence copy_fence = {};
        if (!device->create_fence(0, reshade::api::fence_flags::none, &copy_fence) ||
            !queue->signal(copy_fence, 1) ||
            !device->wait(copy_fence, 1))
        {
            queue->wait_idle();
        }
        device->destroy_fence(copy_fence);
    }

    reshade::api::subresource_data mapped = {};
    if (device->map_texture_region(staging, 0, nullptr, reshade::api::map_access::read_only, &mapped)) {
        std::vector<float> linear_floats(d_w * d_h);
        const uint8_t* src_row = static_cast<const uint8_t*>(mapped.data);

        for (uint32_t y = 0; y < d_h; ++y) {
            const float* src = reinterpret_cast<const float*>(src_row + y * mapped.row_pitch);
            float* dst = linear_floats.data() + y * d_w;
            std::memcpy(dst, src, d_w * sizeof(float));
        }

        device->unmap_texture_region(staging, 0);
        device->destroy_resource(staging);

        bool is_flat = false;
        float min_val = 1.0f, max_val = 0.0f;
        depth_file::sanitize_and_analyze(linear_floats.data(), linear_floats.size(), is_flat, min_val, max_val);

        float far_plane = 1000.0f;
        char far_str[32] = "";
        if (runtime->get_preprocessor_definition("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", far_str)) {
            try { far_plane = std::stof(far_str); } catch (...) { far_plane = 1000.0f; }
        }

        auto now = std::chrono::system_clock::now();
        SidecarDepthHeader header = {};
        header.encoding = static_cast<uint16_t>(m_depth_bit_depth);
        header.width = d_w;
        header.height = d_h;
        header.flags = is_flat ? (kSidecarFlagValid | kSidecarFlagFlat) : kSidecarFlagValid;
        header.far_plane_used = far_plane;
        header.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        strcpy_s(header.game_name, "GameCapture");

        std::string err;
        bool sldepth_ok = depth_file::write_sidecar(sidecar_path, header, linear_floats.data(), linear_floats.size(), err);

        if (sldepth_ok) {
            log("INFO", "Successfully saved depth sidecar: " + sidecar_path +
                        " (" + std::to_string(d_w) + "x" + std::to_string(d_h) +
                        ", min=" + std::to_string(min_val) + ", max=" + std::to_string(max_val) +
                        (is_flat ? ", FLAT/UNIFORM DETECTED)" : ")"));

            m_last_capture_w = d_w;
            m_last_capture_h = d_h;
            m_last_min_depth = min_val;
            m_last_max_depth = max_val;
            m_last_is_flat = is_flat;
            m_last_saved_sidecar_path = sidecar_path;
            m_last_capture_status = is_flat ? "Captured (Warning: Depth was flat)" : "Captured OK with Full 3D Depth!";
            m_last_capture_success = true;
            return true;
        } else {
            log("ERROR", "Failed to write sidecar file: " + err);
            m_last_capture_status = "Failed: " + err;
            return false;
        }
    } else {
        device->destroy_resource(staging);
        log("ERROR", "Failed to map depth staging buffer");
        m_last_capture_status = "Failed: map staging buffer";
        return false;
    }
}

void CaptureManager::on_reshade_finish_effects(
    reshade::api::effect_runtime* runtime,
    reshade::api::command_list* /*cmd_list*/,
    reshade::api::resource_view /*rtv*/,
    reshade::api::resource_view /*rtv_srgb*/)
{
    if (!runtime) return;

    load_notice_state(runtime);

    if (!m_notice_dismissed && !m_overlay_opened_once) {
        static uint32_t s_frame_delay = 0;
        s_frame_delay++;
        if (s_frame_delay > 90) { // wait ~1.5s after game loads
            m_overlay_opened_once = true;
            runtime->open_overlay(true, reshade::api::input_source::keyboard);
        }
    }
}

void CaptureManager::set_enabled(reshade::api::effect_runtime* runtime, bool enabled) {
    m_enabled = enabled;
    if (runtime) {
        reshade::set_config_value<bool>(runtime, "ShaderLabCapture", "Enabled", m_enabled);
    }
}

void CaptureManager::on_reshade_screenshot(reshade::api::effect_runtime* runtime, const char* path) {
    if (!m_enabled) {
        log("INFO", "ReShade screenshot ignored because ShaderLab Capture add-on is disabled");
        return;
    }
    if (!path || !*path) return;

    log("INFO", "ReShade screenshot event received for: " + std::string(path));
    m_last_saved_png_path = path;

    embed_depth_in_png(runtime, path);
}

void CaptureManager::load_notice_state(reshade::api::effect_runtime* runtime) {
    if (m_notice_checked) return;
    m_notice_checked = true;
    reshade::get_config_value<bool>(runtime, "ShaderLabCapture", "NoticeDismissed", m_notice_dismissed);
    m_notice_open = !m_notice_dismissed;
    reshade::get_config_value<bool>(runtime, "ShaderLabCapture", "Enabled", m_enabled);
    if (!reshade::get_config_value<int>(runtime, "ShaderLabCapture", "BitDepth", m_depth_bit_depth)) {
        bool export_16bit = true;
        if (reshade::get_config_value<bool>(runtime, "ShaderLabCapture", "Export16Bit", export_16bit)) {
            m_depth_bit_depth = export_16bit ? 1 : 0;
        }
    }
    m_depth_bit_depth = std::clamp(m_depth_bit_depth, 0, 3);

    bool standalone = true;
    if (reshade::get_config_value<bool>(runtime, "ShaderLabCapture", "StandaloneTab", standalone)) {
        if (standalone != is_standalone_tab()) {
            set_standalone_tab(runtime, standalone);
        }
    }
}

void CaptureManager::on_reshade_overlay_frame(reshade::api::effect_runtime* runtime) {
    if (!runtime) return;

    load_notice_state(runtime);
    if (!m_notice_open) return;

    // shown once, save it on first draw, so close counts as seen
    if (!m_notice_dismissed) {
        m_notice_dismissed = true;
        reshade::set_config_value<bool>(runtime, "ShaderLabCapture", "NoticeDismissed", true);
    }

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2((std::min)(ImGui::GetFontSize() * 32.0f, io.DisplaySize.x - 40.0f), 0.0f), ImGuiCond_Appearing);

    bool open = true;
    if (ImGui::Begin("ShaderLab Capture - Depth Data##FirstRunNotice", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Screenshots taken with this add-on embed full 3D scene depth data. "
            "To view, relight, or edit it, you need ShaderLab, a free image editor using ReShade shaders.\n\n"
            "Even without it, your screenshots still open normally anywhere, they just have an extra few MBs."
        );
        ImGui::Spacing();

        if (ImGui::Button("Download ShaderLab (.zip)")) {
            ShellExecuteW(nullptr, L"open", L"https://github.com/NotRayST/ShaderLab/releases/latest/download/ShaderLab_Windows.zip", nullptr, nullptr, SW_SHOW);
            open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("ShaderLab GitHub")) {
            ShellExecuteW(nullptr, L"open", L"https://github.com/NotRayST/ShaderLab#readme", nullptr, nullptr, SW_SHOW);
        }
    }
    ImGui::End();  // must run even when Begin() returns false

    if (!open) m_notice_open = false;
}

static std::wstring find_shaderlab_executable() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ShaderLab", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH] = {};
        DWORD sz = sizeof(buf);
        DWORD type = REG_SZ;
        if (RegQueryValueExW(hKey, L"ExecutablePath", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &sz) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            if (buf[0] != L'\0' && fs::exists(buf)) {
                return buf;
            }
        } else {
            RegCloseKey(hKey);
        }
    }
    return L"";
}

static std::string get_screenshot_key_name(reshade::api::effect_runtime* runtime) {
    if (!runtime) return "PrintScreen key or your configured key";

    char buf[128] = { 0 };
    size_t sz = sizeof(buf);
    // reshade::get_config_value returns elements separated by '\0'
    if (!reshade::get_config_value(runtime, "INPUT", "KeyScreenshot", buf, &sz) || sz == 0) {
        return "Print Screen"; // Default ReShade key
    }

    unsigned int keys[4] = { 0, 0, 0, 0 };
    const char* p = buf;
    for (int i = 0; i < 4 && p < buf + sz && *p; ++i) {
        try {
            keys[i] = static_cast<unsigned int>(std::stoul(p));
        } catch (...) {
            keys[i] = 0;
        }
        p += strlen(p) + 1;
    }

    if (keys[0] == 0) {
        return "PrintScreen key or your configured key";
    }

    static const char* const keyboard_keys[256] = {
        "", "Left Mouse", "Right Mouse", "Cancel", "Middle Mouse", "X1 Mouse", "X2 Mouse", "", "Backspace", "Tab", "", "", "Clear", "Enter", "", "",
        "Shift", "Control", "Alt", "Pause", "Caps Lock", "", "", "", "", "", "", "Escape", "", "", "", "",
        "Space", "Page Up", "Page Down", "End", "Home", "Left Arrow", "Up Arrow", "Right Arrow", "Down Arrow", "Select", "", "", "Print Screen", "Insert", "Delete", "Help",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "", "", "", "", "", "",
        "", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
        "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "Left Windows", "Right Windows", "Apps", "", "Sleep",
        "Numpad 0", "Numpad 1", "Numpad 2", "Numpad 3", "Numpad 4", "Numpad 5", "Numpad 6", "Numpad 7", "Numpad 8", "Numpad 9", "Numpad *", "Numpad +", "", "Numpad -", "Numpad Decimal", "Numpad /",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13", "F14", "F15", "F16",
        "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24", "", "", "", "", "", "", "", "",
        "Num Lock", "Scroll Lock", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
        "Left Shift", "Right Shift", "Left Control", "Right Control", "Left Menu", "Right Menu", "Browser Back", "Browser Forward", "Browser Refresh", "Browser Stop", "Browser Search", "Browser Favorites", "Browser Home", "Volume Mute", "Volume Down", "Volume Up",
        "Next Track", "Previous Track", "Media Stop", "Media Play/Pause", "Mail", "Media Select", "Launch App 1", "Launch App 2", "", "", "OEM ;", "OEM +", "OEM ,", "OEM -", "OEM .", "OEM /",
        "OEM ~", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
        "", "", "", "", "", "", "", "", "", "", "", "OEM [", "OEM \\", "OEM ]", "OEM '", "OEM 8",
        "", "", "OEM <", "", "", "", "", "", "", "", "", "", "", "", "", "",
        "", "", "", "", "", "", "Attn", "CrSel", "ExSel", "Erase EOF", "Play", "Zoom", "", "PA1", "OEM Clear", ""
    };

    std::string key_str;
    if (keys[1]) key_str += "Ctrl + ";
    if (keys[2]) key_str += "Shift + ";
    if (keys[3]) key_str += "Alt + ";

    if (keys[0] < 256 && keyboard_keys[keys[0]][0] != '\0') {
        key_str += keyboard_keys[keys[0]];
    } else {
        char name[64] = { 0 };
        UINT scanCode = MapVirtualKeyA(keys[0], MAPVK_VK_TO_VSC);
        if (scanCode != 0 && GetKeyNameTextA(scanCode << 16, name, sizeof(name)) > 0) {
            key_str += name;
        } else {
            key_str += "Key(" + std::to_string(keys[0]) + ")";
        }
    }

    return key_str;
}

void CaptureManager::on_draw_overlay(reshade::api::effect_runtime* runtime) {
    load_notice_state(runtime);

    ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "ShaderLab Capture");
    ImGui::Separator();
    ImGui::Spacing();

    bool enabled = m_enabled;
    if (ImGui::Checkbox("Enabled", &enabled)) {
        set_enabled(runtime, enabled);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Screenshot Key detection & instructions
    std::string key_str = get_screenshot_key_name(runtime);
    ImGui::TextWrapped("How to capture: Press %s to capture with embedded 3D depth.", key_str.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.65f, 1.0f));
    ImGui::TextWrapped("Capturing via external overlay software (Steam, Nvidia, Windows Game Bar) will not embed depth.");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Dropdown for bit definition
    const char* bit_depth_items[] = {
        "32-Bit Float (Lossless, ~4 B/px)",
        "16-Bit Int (High Quality, ~2 B/px)",
        "8-Bit Int (Standard, ~1 B/px)",
        "4-Bit Int (Compact, ~0.5 B/px)"
    };
    ImGui::Text("Depth Bit Definition:");
    if (ImGui::Combo("##DepthBitDepth", &m_depth_bit_depth, bit_depth_items, IM_ARRAYSIZE(bit_depth_items))) {
        reshade::set_config_value<int>(runtime, "ShaderLabCapture", "BitDepth", m_depth_bit_depth);
    }

    static const char* const bit_depth_descriptions[] = {
        "Lossless 32-bit floating point. Maximum precision, largest file size (~4x).",
        "16-bit quantized (65,536 levels). Visually indistinguishable from 32-bit (~2x size).",
        "8-bit normalized (256 levels). Balanced quality with small file size (~1x size).",
        "4-bit quantized (16 levels). Extremely compact depth payload (~0.5x size)."
    };
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
    ImGui::TextWrapped("%s", bit_depth_descriptions[m_depth_bit_depth]);
    ImGui::PopStyleColor();

    ImGui::Spacing();
    bool embed_in_addons = !is_standalone_tab();
    if (ImGui::Checkbox("Embed into Add-ons tab", &embed_in_addons)) {
        set_standalone_tab(runtime, !embed_in_addons);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!m_last_saved_png_path.empty() && m_last_capture_success) {
        fs::path p(m_last_saved_png_path);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
        ImGui::Text("Depth saved to %s.", p.filename().string().c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    std::wstring exe = find_shaderlab_executable();
    if (exe.empty()) {
        // Not downloaded yet: auto-download link
        if (ImGui::Button("Download ShaderLab (.zip)", ImVec2(-1, 32))) {
            ShellExecuteW(nullptr, L"open", L"https://github.com/NotRayST/ShaderLab/releases/latest/download/ShaderLab_Windows.zip", nullptr, nullptr, SW_SHOW);
        }
    } else if (m_last_saved_png_path.empty()) {
        // Downloaded, but no capture yet
        if (ImGui::Button("Open ShaderLab", ImVec2(-1, 32))) {
            ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOW);
        }
    } else {
        // Downloaded and captured: open last captured image
        if (ImGui::Button("Open Last Captured Image in ShaderLab", ImVec2(-1, 32))) {
            std::wstring arg = L"\"" + fs::path(m_last_saved_png_path).wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", exe.c_str(), arg.c_str(), nullptr, SW_SHOW);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("ShaderLab Capture v1.2.2  -  by NotRayST");
    ImGui::PopTextWrapPos();
}

