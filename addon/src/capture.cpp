#include "capture.h"
#include "job_queue.h"
#include "hud_composite.h"
#include "before_after.h"
#include "keybinds.h"
#include "overlay_ui.h"
#include "../../common/depth_file.h"
#include "../../common/depth_chunk.h"
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cmath>
#include <windows.h>

#include "../../common/fast_png_zlib.h"
#include "../../common/str_utils.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

namespace fs = std::filesystem;
using str_utils::wide_to_utf8;

static uint32_t s_last_bound_depth_version = 0;


static bool s_depth_peek_active = false;
static bool s_prev_effects_state = true;

static void reset_depth_peek_state(reshade::api::effect_runtime *runtime = nullptr) {
    if (s_depth_peek_active && runtime && s_prev_effects_state) {
        runtime->set_effects_state(true);
    }
    s_depth_peek_active = false;
    s_prev_effects_state = true;
}

static void sync_depth_peek_state(reshade::api::effect_runtime *runtime, SharedControlBlock *block) {
    if (!runtime || !block) return;
    bool want_peek = (block->view_interaction_flags & VIEW_FLAG_DEPTH_PEEK) != 0 && (block->depth_valid != 0);
    if (want_peek) {
        if (!s_depth_peek_active) {
            s_prev_effects_state = runtime->get_effects_state();
            if (s_prev_effects_state) {
                runtime->set_effects_state(false);
            }
            s_depth_peek_active = true;
        }
    } else if (s_depth_peek_active) {
        if (s_prev_effects_state) {
            runtime->set_effects_state(true);
        }
        s_depth_peek_active = false;
    }
}

struct SavedAfUniform {
    reshade::api::effect_uniform_variable var;
    float value;
};
static std::vector<SavedAfUniform> s_accelerated_af_uniforms;

static void accelerate_autofocus(reshade::api::effect_runtime *runtime) {
    if (!runtime) return;
    static const char *kAfSpeedUniforms[] = {
        "fADOF_AutofocusSpeed",
        "AutoFocusTransitionSpeed",
        "fLightDoF_AutoFocusSpeed",
        "fFocusTransitionSpeed",
        "AutoFocusSpeed"
    };

    for (const char *name : kAfSpeedUniforms) {
        reshade::api::effect_uniform_variable var = runtime->find_uniform_variable(nullptr, name);
        if (var.handle != 0) {
            float cur_val = 0.0f;
            runtime->get_uniform_value_float(var, &cur_val, 1);
            if (cur_val > 0.0f && cur_val < 0.99f) {
                s_accelerated_af_uniforms.push_back({ var, cur_val });
                float fast_val = 1.0f;
                runtime->set_uniform_value_float(var, &fast_val, 1);
            }
        }
    }
}

static void restore_autofocus(reshade::api::effect_runtime *runtime) {
    if (!runtime) return;
    for (const auto &item : s_accelerated_af_uniforms) {
        runtime->set_uniform_value_float(item.var, &item.value, 1);
    }
    s_accelerated_af_uniforms.clear();
}

static bool s_finish_effects_fired_this_frame = false;
static bool s_is_loading = false;

bool is_effects_loading() {
    return s_is_loading;
}

void on_reshade_present(reshade::api::effect_runtime *runtime) {
    JobQueueManager &queue_mgr = JobQueueManager::get();
    SharedControlBlock *block = queue_mgr.get_control_block();
    if (!block || !queue_mgr.is_connected()) return;

    block->addon_heartbeat++;

    static uint32_t s_last_save_project_version = 0;
    if (block->request_save_project != s_last_save_project_version) {
        s_last_save_project_version = block->request_save_project;
        try {
            trigger_save_project_workflow(runtime, false);
        } catch (...) {
            queue_mgr.add_log(L"Exception in trigger_save_project_workflow");
        }
        // resync in case extra increments happened while modal loop ran
        s_last_save_project_version = block->request_save_project;
    }

    static uint32_t s_last_save_project_as_version = 0;
    if (block->request_save_project_as != s_last_save_project_as_version) {
        s_last_save_project_as_version = block->request_save_project_as;
        try {
            trigger_save_project_workflow(runtime, true);
        } catch (...) {
            queue_mgr.add_log(L"Exception in trigger_save_project_workflow (as)");
        }
        // resync in case extra increments happened while modal loop ran
        s_last_save_project_as_version = block->request_save_project_as;
    }

    static uint32_t s_last_export_image_version = 0;
    if (block->request_export_image != s_last_export_image_version) {
        s_last_export_image_version = block->request_export_image;
        try {
            trigger_export_workflow(runtime);
        } catch (...) {
            queue_mgr.add_log(L"Exception in trigger_export_workflow");
        }
        s_last_export_image_version = block->request_export_image;
    }

    static uint32_t s_last_export_image_as_version = 0;
    if (block->request_export_image_as != s_last_export_image_as_version) {
        s_last_export_image_as_version = block->request_export_image_as;
        try {
            trigger_export_as_workflow(runtime);
        } catch (...) {
            queue_mgr.add_log(L"Exception in trigger_export_as_workflow");
        }
        s_last_export_image_as_version = block->request_export_image_as;
    }

    bool effects_on = (runtime && runtime->get_effects_state() && !s_depth_peek_active);
    block->effects_enabled = effects_on ? 1u : 0u;

    if (effects_on) {
        if (!s_finish_effects_fired_this_frame) {
            s_is_loading = true;
            block->effects_compiling = 1;
        } else {
            s_is_loading = false;
            block->effects_compiling = 0;
        }
    } else {
        s_is_loading = false;
        block->effects_compiling = 0;
    }
    s_finish_effects_fired_this_frame = false;
    sync_depth_peek_state(runtime, block);

    if (runtime && block->mailbox.seq_request != block->mailbox.seq_handled) {
        uint32_t cmd = block->mailbox.cmd_id;
        if (cmd == static_cast<uint32_t>(IpcCmd::ReshadeToggleEffects)) {
            bool on = (block->mailbox.values[0] > 0.5f);
            runtime->set_effects_state(on);
            block->mailbox.status = 1;
            MemoryBarrier();
            block->mailbox.seq_handled = block->mailbox.seq_request;
        } else if (cmd == static_cast<uint32_t>(IpcCmd::ReshadeReloadEffects)) {
            runtime->reload_effect_next_frame(nullptr);
            block->mailbox.status = 1;
            MemoryBarrier();
            block->mailbox.seq_handled = block->mailbox.seq_request;
        } else if (cmd == static_cast<uint32_t>(IpcCmd::ReshadeToggleOverlay)) {
            bool open = (block->mailbox.values[0] > 0.5f);
            runtime->open_overlay(open, reshade::api::input_source::keyboard);
            block->mailbox.status = 1;
            MemoryBarrier();
            block->mailbox.seq_handled = block->mailbox.seq_request;
        } else if (cmd == static_cast<uint32_t>(IpcCmd::ReshadeSetPreset)) {
            std::string path_u8 = wide_to_utf8(block->mailbox.path);
            runtime->set_current_preset_path(path_u8.c_str());
            if (block->active_project_path[0] != L'\0') block->project_dirty = 1;
            block->mailbox.status = 1;
            MemoryBarrier();
            block->mailbox.seq_handled = block->mailbox.seq_request;
        } else if (cmd == static_cast<uint32_t>(IpcCmd::ReshadeSetTechnique)) {
            struct TechSearch {
                const char *target;
                bool enable;
                bool found;
            } search = { block->mailbox.target, block->mailbox.values[0] > 0.5f, false };

            runtime->enumerate_techniques(nullptr, [](reshade::api::effect_runtime *rt, reshade::api::effect_technique tech, void *data) {
                auto *s = static_cast<TechSearch *>(data);
                char name[128] = {};
                size_t name_sz = sizeof(name);
                rt->get_technique_name(tech, name, &name_sz);
                if (_stricmp(name, s->target) == 0) {
                    rt->set_technique_state(tech, s->enable);
                    s->found = true;
                }
            }, &search);

            if (search.found && block->active_project_path[0] != L'\0') {
                block->project_dirty = 1;
            }
            block->mailbox.status = search.found ? 1 : -1;
            if (!search.found) {
                swprintf_s(block->mailbox.response_text, L"Technique not found");
            }
            MemoryBarrier();
            block->mailbox.seq_handled = block->mailbox.seq_request;
        } else if (cmd == static_cast<uint32_t>(IpcCmd::ReshadeSetUniform)) {
            struct UniSearch {
                const char *target;
                float val[4];
                bool found;
            } search = { block->mailbox.target, { block->mailbox.values[0], block->mailbox.values[1], block->mailbox.values[2], block->mailbox.values[3] }, false };

            runtime->enumerate_uniform_variables(nullptr, [](reshade::api::effect_runtime *rt, reshade::api::effect_uniform_variable var, void *data) {
                auto *s = static_cast<UniSearch *>(data);
                char name[128] = {};
                size_t name_sz = sizeof(name);
                rt->get_uniform_variable_name(var, name, &name_sz);
                if (_stricmp(name, s->target) == 0) {
                    rt->set_uniform_value_float(var, s->val, 4);
                    s->found = true;
                }
            }, &search);

            if (search.found && block->active_project_path[0] != L'\0') {
                block->project_dirty = 1;
            }

            block->mailbox.status = search.found ? 1 : -1;
            if (!search.found) {
                swprintf_s(block->mailbox.response_text, L"Uniform variable not found");
            }
            MemoryBarrier();
            block->mailbox.seq_handled = block->mailbox.seq_request;
        }
    }
}

void on_reshade_begin_effects(
    reshade::api::effect_runtime *runtime,
    reshade::api::command_list *cmd_list,
    reshade::api::resource_view rtv,
    reshade::api::resource_view)
{
    JobQueueManager &queue_mgr = JobQueueManager::get();
    SharedControlBlock *block = queue_mgr.get_control_block();
    if (!block || !queue_mgr.is_connected()) return;

    sync_depth_peek_state(runtime, block);
    before_after_capture_pre(block, runtime, cmd_list, rtv);

    if (block->depth_version != s_last_bound_depth_version) {
        s_last_bound_depth_version = block->depth_version;
        if (block->depth_valid != 0 && block->depth_srv_ptr != 0) {
            reshade::api::resource_view depth_srv_view{ block->depth_srv_ptr };
            runtime->update_texture_bindings("DEPTH", depth_srv_view, depth_srv_view);
        } else {
            runtime->update_texture_bindings("DEPTH", reshade::api::resource_view{ 0 }, reshade::api::resource_view{ 0 });
        }
    }
}

void on_init_effect_runtime(reshade::api::effect_runtime *runtime) {
    char p[MAX_PATH] = {};
    size_t sz = sizeof(p);
    reshade::get_config_value(runtime, "GENERAL", "IntermediateCachePath", p, &sz);
    if (!p[0] || strstr(p, "\\Temp\\") || strstr(p, "\\temp\\") || (strstr(p, "ShaderCache") && !strstr(p, "common"))) {
        std::error_code ec;
        fs::create_directories("common/ShaderCache", ec);
        reshade::set_config_value(runtime, "GENERAL", "IntermediateCachePath", ".\\common\\ShaderCache");
    }

    s_finish_effects_fired_this_frame = false;
    s_is_loading = true;
    JobQueueManager &queue_mgr = JobQueueManager::get();
    SharedControlBlock *block = queue_mgr.get_control_block();
    if (block && queue_mgr.is_connected()) {
        block->effects_compiling = 1;
        if (runtime && block->requested_preset_path[0] != L'\0') {
            std::string target_preset_u8 = wide_to_utf8(block->requested_preset_path);
            if (!target_preset_u8.empty()) {
                char current_preset[MAX_PATH] = {};
                size_t path_size = sizeof(current_preset);
                runtime->get_current_preset_path(current_preset, &path_size);
                if (_stricmp(current_preset, target_preset_u8.c_str()) != 0) {
                    runtime->set_current_preset_path(target_preset_u8.c_str());
                }
            }
        }
    }
}

void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime) {
    s_finish_effects_fired_this_frame = false;
    s_is_loading = true;
    reset_depth_peek_state(runtime);

    JobQueueManager &queue_mgr = JobQueueManager::get();
    SharedControlBlock *block = queue_mgr.get_control_block();
    if (block && queue_mgr.is_connected()) {
        block->effects_compiling = 1;
    }
}

void on_reshade_reloaded_effects(reshade::api::effect_runtime *runtime) {
    reset_depth_peek_state(runtime);

    JobQueueManager &queue_mgr = JobQueueManager::get();
    SharedControlBlock *block = queue_mgr.get_control_block();
    if (!block || !queue_mgr.is_connected()) return;

    if (block->depth_valid != 0 && block->depth_srv_ptr != 0) {
        reshade::api::resource_view depth_srv_view{ block->depth_srv_ptr };
        runtime->update_texture_bindings("DEPTH", depth_srv_view, depth_srv_view);
    }
}

void on_reshade_finish_effects(
    reshade::api::effect_runtime *runtime,
    reshade::api::command_list *cmd_list,
    reshade::api::resource_view rtv,
    reshade::api::resource_view)
{
    JobQueueManager &queue_mgr = JobQueueManager::get();
    queue_mgr.update();

    SharedControlBlock *block = queue_mgr.get_control_block();
    if (!block || !queue_mgr.is_connected()) return;

    s_finish_effects_fired_this_frame = true;
    s_is_loading = false;
    block->effects_compiling = 0;
    block->effects_enabled = 1;
    block->effects_ready++;


    if (block->keybind_version == 0) {
        keybinds::sync_to_ipc(block);
    }
    block->fine_tune_vk = keybinds::fine_tune_vk();

    static uint32_t s_last_preset_version = 0;
    if (block->requested_preset_version != s_last_preset_version && block->requested_preset_path[0] != L'\0') {
        s_last_preset_version = block->requested_preset_version;
        std::string target_preset_u8 = wide_to_utf8(block->requested_preset_path);
        if (!target_preset_u8.empty() && runtime) {
            char current_preset[MAX_PATH] = {};
            size_t path_size = sizeof(current_preset);
            runtime->get_current_preset_path(current_preset, &path_size);
            if (_stricmp(current_preset, target_preset_u8.c_str()) != 0) {
                block->effects_compiling = 1;
                s_is_loading = true;
                s_finish_effects_fired_this_frame = false;
                runtime->set_current_preset_path(target_preset_u8.c_str());
            }
        }
    }

    static bool s_first_run_resolved = false;
    if (!s_first_run_resolved) {
        s_first_run_resolved = true;
        int first_run_done = 0;
        if (!reshade::get_config_value(runtime, "ShaderLab", "FirstRunDone", first_run_done) || first_run_done != 1) {
            reshade::set_config_value(runtime, "ShaderLab", "FirstRunDone", 1);
            block->first_run_nudge = 1;
        }

    }

    block->addon_heartbeat++;

    static ExportState s_prev_export_state = ExportState::Idle;
    if (s_prev_export_state != block->export_state) {
        if (block->export_state == ExportState::Rendering) {
            accelerate_autofocus(runtime);
        } else if (s_prev_export_state == ExportState::Rendering ||
                   block->export_state == ExportState::Capturing ||
                   block->export_state == ExportState::Done ||
                   block->export_state == ExportState::Idle) {
            restore_autofocus(runtime);
        }
        s_prev_export_state = block->export_state;
    }

    if (block->export_state == ExportState::Idle) {
        before_after_composite(block, runtime, cmd_list, rtv);
        composite_hud(block, runtime, cmd_list, rtv);
    }

    if (block->export_state != ExportState::Capturing) {
        return;
    }

    reshade::api::device *device = runtime->get_device();
    reshade::api::command_queue *queue = runtime->get_command_queue();
    if (!device || !queue || !cmd_list) {
        block->export_state = ExportState::Failed;
        return;
    }

    reshade::api::resource back_buffer = device->get_resource_from_view(rtv);
    if (back_buffer.handle == 0) {
        block->export_error = IPC_ERR_STAGING_FAILED;
        block->export_state = ExportState::Failed;
        queue_mgr.add_log(L"Capture failed: could not resolve back buffer resource from view");
        return;
    }

    const reshade::api::resource_desc desc = device->get_resource_desc(back_buffer);
    const reshade::api::format format = reshade::api::format_to_default_typed(desc.texture.format, 0);

    reshade::api::resource staging = {};
    if (!device->create_resource(
            reshade::api::resource_desc(
                desc.texture.width, desc.texture.height, 1, 1,
                format, 1,
                reshade::api::memory_heap::readback,
                reshade::api::resource_usage::copy_dest),
            nullptr,
            reshade::api::resource_usage::copy_dest,
            &staging)) {
            // TODO: fails on some drivers and I have no idea why yet, need to actually debug this
            block->export_error = IPC_ERR_STAGING_FAILED;
            block->export_state = ExportState::Failed;
            queue_mgr.add_log(L"Capture failed: create staging resource failed");
            return;
    }

    cmd_list->barrier(back_buffer,
        reshade::api::resource_usage::render_target,
        reshade::api::resource_usage::copy_source);

    cmd_list->copy_resource(back_buffer, staging);

    cmd_list->barrier(back_buffer,
        reshade::api::resource_usage::copy_source,
        reshade::api::resource_usage::render_target);

    queue->flush_immediate_command_list();

    reshade::api::subresource_data mapped = {};
    if (device->map_texture_region(staging, 0, nullptr, reshade::api::map_access::read_only, &mapped)) {
        fs::path out_p(block->export_output_path);
        if (out_p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(out_p.parent_path(), ec);
        }

        std::string out_path_utf8 = wide_to_utf8(block->export_output_path);

        bool is_bgra = (format == reshade::api::format::b8g8r8a8_unorm ||
                        format == reshade::api::format::b8g8r8a8_unorm_srgb ||
                        format == reshade::api::format::b8g8r8a8_typeless);

        uint32_t w = desc.texture.width;
        uint32_t h = desc.texture.height;
        std::vector<uint8_t> rgba_pixels(w * h * 4);
        const uint8_t *src_row = static_cast<const uint8_t *>(mapped.data);

        if (is_bgra) {
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t *src = src_row + (y * mapped.row_pitch);
                uint8_t *dst = rgba_pixels.data() + (y * w * 4);
                for (uint32_t x = 0; x < w; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 255; // shaders leave alpha at 0, force it opaque or images look invisible, why cant anything be freaking normal here?
                }
            }
        } else {
            for (uint32_t y = 0; y < h; ++y) {
                const uint32_t *src32 = reinterpret_cast<const uint32_t *>(src_row + (y * mapped.row_pitch));
                uint32_t *dst32 = reinterpret_cast<uint32_t *>(rgba_pixels.data() + (y * w * 4));
                for (uint32_t x = 0; x < w; ++x) {
                    dst32[x] = src32[x] | 0xFF000000;
                }
            }
        }

        // stbi write here according to extension
        std::string ext = fs::path(block->export_output_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        int ok = 0;
        if (ext == ".jpg" || ext == ".jpeg") {
            ok = stbi_write_jpg(out_path_utf8.c_str(),
                                static_cast<int>(w),
                                static_cast<int>(h),
                                4,
                                rgba_pixels.data(),
                                95);
        } else {
            stbi_write_force_png_filter = 0;
            ok = stbi_write_png(out_path_utf8.c_str(),
                                static_cast<int>(w),
                                static_cast<int>(h),
                                4,
                                rgba_pixels.data(),
                                static_cast<int>(w * 4));
        }

        device->unmap_texture_region(staging, 0);

        if (ok != 0) {
            bool req_embed = (block->export_flags & EXPORT_FLAG_EMBED_DEPTH) != 0;
            bool req_sidecar = (block->export_flags & EXPORT_FLAG_DEPTH_SIDECAR) != 0;
            if ((req_embed || req_sidecar) && block->depth_valid != 0) {
                fs::path in_p(block->export_input_path);
                fs::path in_sidecar = in_p;
                in_sidecar.replace_extension(".sldepth");

                std::vector<float> linear_floats;
                uint32_t dw = 0, dh = 0;
                float far_plane = (block->depth_far_plane > 0.0f) ? block->depth_far_plane : 1000.0f;
                bool found = false;

                if (fs::exists(in_sidecar)) {
                    SidecarDepthHeader s_hdr = {};
                    std::string err;
                    if (depth_file::read_sidecar(wide_to_utf8(in_sidecar.wstring().c_str()), s_hdr, linear_floats, err)) {
                        dw = s_hdr.width;
                        dh = s_hdr.height;
                        far_plane = s_hdr.far_plane_used;
                        found = true;
                    }
                }

                if (!found && fs::exists(in_p)) {
                    std::ifstream in_f(in_p, std::ios::binary | std::ios::ate);
                    if (in_f.is_open()) {
                        size_t sz = in_f.tellg();
                        in_f.seekg(0, std::ios::beg);
                        std::vector<uint8_t> buf(sz);
                        in_f.read(reinterpret_cast<char *>(buf.data()), sz);
                        in_f.close();

                        DepthMapHeader c_hdr = {};
                        std::string err;
                        if (depth_chunk::extract_sldp(buf.data(), buf.size(), c_hdr, linear_floats, err)) {
                            dw = c_hdr.width;
                            dh = c_hdr.height;
                            far_plane = c_hdr.far_plane;
                            found = true;
                        }
                    }
                }

                if (found && linear_floats.size() == dw * dh) {
                    std::string out_ext = out_p.extension().string();
                    std::transform(out_ext.begin(), out_ext.end(), out_ext.begin(), [](unsigned char c) { return (char)::tolower(c); });

                    if (req_embed) {
                        if (out_ext == ".png") {
                            DepthMapHeader c_hdr = {};
                            c_hdr.magic = kDepthMagicSLD1;
                            c_hdr.version = kDepthVersion1;
                            c_hdr.encoding = 0;
                            c_hdr.width = dw;
                            c_hdr.height = dh;
                            c_hdr.flags = kDepthFlagValid;
                            c_hdr.near_plane = 1.0f;
                            c_hdr.far_plane = far_plane;
                            c_hdr.raw_byte_size = static_cast<uint32_t>(dw * dh * sizeof(float));
                            c_hdr.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count());
                            strcpy_s(c_hdr.game_name, "ShaderLabExport");

                            std::string inj_err;
                            if (depth_chunk::inject_sldp(out_path_utf8, c_hdr, linear_floats.data(), linear_floats.size(), inj_err)) {
                                queue_mgr.add_log(L"Embedded depth chunk into exported PNG");
                            }
} else {
                req_sidecar = true;
            }
                    }

                    if (req_sidecar) {
                        fs::path out_sidecar = out_p;
                        out_sidecar.replace_extension(".sldepth");
                        SidecarDepthHeader s_hdr = {};
                        s_hdr.magic = kSidecarMagic;
                        s_hdr.version = kSidecarVersion;
                        s_hdr.encoding = 0;
                        s_hdr.width = dw;
                        s_hdr.height = dh;
                        s_hdr.flags = kSidecarFlagValid;
                        s_hdr.far_plane_used = far_plane;
                        s_hdr.raw_byte_size = static_cast<uint32_t>(dw * dh * sizeof(float));
                        s_hdr.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                        strcpy_s(s_hdr.game_name, "ShaderLabExport");

                        std::string w_err;
                        if (depth_file::write_sidecar(wide_to_utf8(out_sidecar.wstring().c_str()), s_hdr, linear_floats.data(), linear_floats.size(), w_err)) {
                            queue_mgr.add_log(L"Exported depth companion: " + out_sidecar.filename().wstring());
                        }
                    }
                }
            } else {
                fs::path out_sidecar = out_p;
                out_sidecar.replace_extension(".sldepth");
                if (fs::exists(out_sidecar)) {
                    std::error_code ec;
                    fs::remove(out_sidecar, ec);
                }
            }

            block->export_error = IPC_OK;
            block->export_state = ExportState::Done;
            std::wstring status_msg = L"Export complete: " + fs::path(block->export_output_path).filename().wstring();
            wcsncpy_s(block->export_status, kMaxPathW, status_msg.c_str(), _TRUNCATE);

            std::wstring log_msg = L"Export complete: " + std::wstring(block->export_output_path) +
                                   L" (" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L")";
            if (block->export_converged) {
                wchar_t conv_buf[96];
                swprintf_s(conv_buf, L" (auto-converged delta=%.4f)", block->export_last_delta);
                log_msg += conv_buf;
            }
            queue_mgr.add_log(log_msg);
        } else {
            block->export_error = IPC_ERR_WRITE_FAILED;
            block->export_state = ExportState::Failed;
            wcsncpy_s(block->export_status, kMaxPathW, L"Export failed (PNG write error)", _TRUNCATE);
            queue_mgr.add_log(L"Export failed: stbi_write_png failed for " + std::wstring(block->export_output_path));
        }
    } else {
        block->export_error = IPC_ERR_STAGING_FAILED;
        block->export_state = ExportState::Failed;
        wcsncpy_s(block->export_status, kMaxPathW, L"Export failed (Staging map error)", _TRUNCATE);
        queue_mgr.add_log(L"Export failed: map_texture_region failed");
    }

    device->destroy_resource(staging);
}
