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

        auto now = std::chrono::system_clock::now();
        DepthMapHeader header = {};
        header.magic = kDepthMagicSLD1;
        header.version = kDepthVersion1;
        header.encoding = m_export_16bit ? 1 : 0;
        header.width = d_w;
        header.height = d_h;
        header.flags = is_flat ? 0 : kDepthFlagValid;
        header.near_plane = 1.0f;
        header.far_plane = far_plane;
        header.raw_byte_size = static_cast<uint32_t>(d_w * d_h * (m_export_16bit ? sizeof(uint16_t) : sizeof(float)));
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
        header.encoding = m_export_16bit ? 1 : 0;
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
    reshade::api::effect_runtime* /*runtime*/,
    reshade::api::command_list* /*cmd_list*/,
    reshade::api::resource_view /*rtv*/,
    reshade::api::resource_view /*rtv_srgb*/)
{
}

void CaptureManager::on_reshade_screenshot(reshade::api::effect_runtime* runtime, const char* path) {
    if (!path || !*path) return;

    log("INFO", "ReShade screenshot event received for: " + std::string(path));
    m_last_saved_png_path = path;

    embed_depth_in_png(runtime, path);
}

void CaptureManager::on_draw_overlay(reshade::api::effect_runtime* /*runtime*/) {
    ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "ShaderLab Capture");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped("How to capture: Press Printscreen or F12, or what you changed the reshade screenshot button to, screenshotting using other methods won't work.");

    ImGui::Spacing();
    ImGui::Checkbox("Export 16-Bit Depth (Recommended for file size)", &m_export_16bit);
}

