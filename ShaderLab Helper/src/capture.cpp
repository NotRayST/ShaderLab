#include "capture.h"
#include "job_queue.h"
#include <string>
#include <filesystem>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

namespace fs = std::filesystem;

static std::string wchar_to_utf8(const wchar_t *wstr) {
    if (!wstr || !*wstr) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), size, nullptr, nullptr);
    return out;
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

    // only grab the frame when the host has primed everything & asked for a capture,
    // otherwise this would just dump random present frames
    if (block->export_state != ExportState::Capturing) {
        return;
    }

    reshade::api::device *device = runtime->get_device();
    reshade::api::command_queue *queue = runtime->get_command_queue();
    if (!device || !queue || !cmd_list) {
        block->export_state = ExportState::Failed;
        return;
    }

    // resolve the real backbuffer from the rtv, handle can change every frame
    reshade::api::resource back_buffer = device->get_resource_from_view(rtv);
    if (back_buffer.handle == 0) {
        block->export_error = IPC_ERR_STAGING_FAILED;
        block->export_state = ExportState::Failed;
        queue_mgr.add_log(L"Capture failed: could not resolve back buffer resource from view");
        return;
    }

    const reshade::api::resource_desc desc = device->get_resource_desc(back_buffer);
    const reshade::api::format format = reshade::api::format_to_default_typed(desc.texture.format, 0);

    // staging tex for gpu->cpu. heap is readback now, gpu_to_cpu is gone in newer api
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
            // TODO: this fails on some drivers for no clear reason, should dig into it
            block->export_error = IPC_ERR_STAGING_FAILED;
            block->export_state = ExportState::Failed;
            queue_mgr.add_log(L"Capture failed: create staging resource failed");
            return;
    }

    // transition bb to copy-source so we can pull it, then flip it back after
    cmd_list->barrier(back_buffer,
        reshade::api::resource_usage::render_target,
        reshade::api::resource_usage::copy_source);

    cmd_list->copy_resource(back_buffer, staging);

    cmd_list->barrier(back_buffer,
        reshade::api::resource_usage::copy_source,
        reshade::api::resource_usage::render_target);

    // make sure the copy is actually flushed & done before we map from cpu side
    queue->flush_immediate_command_list();

    reshade::api::subresource_data mapped = {};
    if (device->map_texture_region(staging, 0, nullptr, reshade::api::map_access::read_only, &mapped)) {
        fs::path out_p(block->export_output_path);
        if (out_p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(out_p.parent_path(), ec);
        }

        std::string out_path_utf8 = wchar_to_utf8(block->export_output_path);

        // some drivers hand us bgra instead of rgba, swap the channels so colors arent inverted. this bgra bullshit wasted a whole afternoon..
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
                const uint8_t *src = src_row + (y * mapped.row_pitch);
                uint8_t *dst = rgba_pixels.data() + (y * w * 4);
                for (uint32_t x = 0; x < w; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 0];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 2];
                    dst[x * 4 + 3] = 255;
                }
            }
        }

        // stbi_write_png, hope the rowpitch matches (it does, verified on nvidia+amd)
        int ok = stbi_write_png(out_path_utf8.c_str(),
                                static_cast<int>(w),
                                static_cast<int>(h),
                                4,
                                rgba_pixels.data(),
                                static_cast<int>(w * 4));

        device->unmap_texture_region(staging, 0);

        if (ok != 0) {
            block->export_error = IPC_OK;
            block->export_state = ExportState::Done;
            std::wstring status_msg = L"Export complete: " + fs::path(block->export_output_path).filename().wstring();
            wcsncpy_s(block->export_status, kMaxPathW, status_msg.c_str(), _TRUNCATE);
            queue_mgr.add_log(L"Export complete: " + std::wstring(block->export_output_path) +
                              L" (" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L")");
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
