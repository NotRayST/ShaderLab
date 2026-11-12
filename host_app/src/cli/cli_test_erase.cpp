#include "cli_test_erase.h"
#include "../gfx_device.h"
#include "../erase_tool.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <set>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

struct MaskGpuStats {
    bool ok = false;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    size_t nonzero_pixels = 0;
    size_t spine_missing_pixels = 0;
    std::set<uint32_t> unique_nonzero_colors;
    std::string error;
};

static MaskGpuStats readback_mask_gpu(
    ID3D11Device *dev,
    ID3D11DeviceContext *ctx,
    ID3D11ShaderResourceView *srv,
    int spine_y,
    int spine_x0,
    int spine_x1)
{
    MaskGpuStats st;
    if (!dev || !ctx || !srv) {
        st.error = "Null device, context, or mask SRV";
        return st;
    }

    ComPtr<ID3D11Resource> res;
    srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex)) || !tex) {
        st.error = "Mask SRV resource is not ID3D11Texture2D";
        return st;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    tex->GetDesc(&desc);
    st.width = desc.Width;
    st.height = desc.Height;
    st.format = desc.Format;

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&staging_desc, nullptr, &staging)) || !staging) {
        st.error = "Failed to create staging texture for GPU readback";
        return st;
    }

    ctx->CopyResource(staging.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        st.error = "Failed to map staging texture";
        return st;
    }

    const uint8_t *base = static_cast<const uint8_t *>(mapped.pData);
    bool is_r8 = (desc.Format == DXGI_FORMAT_R8_UNORM);
    bool is_rgba8 = (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);

    if (!is_r8 && !is_rgba8) {
        ctx->Unmap(staging.Get(), 0);
        st.error = "Unsupported mask texture DXGI format";
        return st;
    }

    for (uint32_t y = 0; y < desc.Height; ++y) {
        const uint8_t *row = base + static_cast<size_t>(y) * mapped.RowPitch;
        if (is_rgba8) {
            const uint32_t *px = reinterpret_cast<const uint32_t *>(row);
            for (uint32_t x = 0; x < desc.Width; ++x) {
                uint32_t c = px[x];
                if (c != 0) {
                    st.nonzero_pixels++;
                    if (st.unique_nonzero_colors.size() < 32) {
                        st.unique_nonzero_colors.insert(c);
                    }
                }
                if (spine_x0 <= spine_x1 && static_cast<int>(y) == spine_y &&
                    static_cast<int>(x) >= spine_x0 && static_cast<int>(x) <= spine_x1) {
                    if (c == 0) st.spine_missing_pixels++;
                }
            }
        } else {
            for (uint32_t x = 0; x < desc.Width; ++x) {
                uint8_t v = row[x];
                if (v != 0) {
                    st.nonzero_pixels++;
                    if (st.unique_nonzero_colors.size() < 32) {
                        st.unique_nonzero_colors.insert(static_cast<uint32_t>(v));
                    }
                }
                if (spine_x0 <= spine_x1 && static_cast<int>(y) == spine_y &&
                    static_cast<int>(x) >= spine_x0 && static_cast<int>(x) <= spine_x1) {
                    if (v == 0) st.spine_missing_pixels++;
                }
            }
        }
    }

    ctx->Unmap(staging.Get(), 0);
    st.ok = true;
    return st;
}

} // namespace

int CliTestErase::execute(const CliOptions &opts) {
    GfxDevice gfx;
    if (!gfx.initialize(L"ShaderLab - Erase Perf & Mask Validator", 640, 360, 640, 360, false)) {
        std::cerr << "[TestErase] Failed to initialize headless D3D11 device\n";
        return 1;
    }

    ID3D11Device *dev = gfx.get_device();
    ID3D11DeviceContext *ctx = gfx.get_context();
    EraseTool &tool = EraseTool::get();

    const uint32_t W = 3840;
    const uint32_t H = 2160;
    tool.clear_mask();
    tool.ensure_mask_size(W, H);
    tool.update_gpu_texture(dev); // Create initial 4K GPU texture once

    // Test 1: Continuous 4K stroke latency & GPU upload latency
    const int kSteps = 20;
    const float x_start = 1600.0f;
    const float x_end = 2000.0f;
    const float y_pos = 1080.0f;
    const float radius = 24.0f;

    tool.push_undo();
    double max_step_ms = 0.0;
    double total_step_ms = 0.0;

    ImVec2 prev_pt(x_start, y_pos);
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        tool.draw_brush_dab(static_cast<int>(prev_pt.x), static_cast<int>(prev_pt.y), static_cast<int>(radius), 255);
        tool.update_gpu_texture(dev);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        max_step_ms = (std::max)(max_step_ms, ms);
        total_step_ms += ms;
    }

    for (int s = 1; s <= kSteps; ++s) {
        float t = static_cast<float>(s) / static_cast<float>(kSteps);
        ImVec2 curr_pt(x_start + (x_end - x_start) * t, y_pos);

        auto t0 = std::chrono::high_resolution_clock::now();
        tool.draw_brush_line(prev_pt, curr_pt, radius, false);
        tool.update_gpu_texture(dev);
        auto t1 = std::chrono::high_resolution_clock::now();

        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        max_step_ms = (std::max)(max_step_ms, ms);
        total_step_ms += ms;
        prev_pt = curr_pt;
    }

    double avg_step_ms = total_step_ms / static_cast<double>(kSteps + 1);
    size_t white_after_stroke = tool.get_mask_white_px();

    // Readback GPU texture to verify uniformity and continuity (no ribbing, no dots)
    MaskGpuStats gpu_stroke = readback_mask_gpu(
        dev, ctx, tool.get_mask_srv(),
        static_cast<int>(y_pos), static_cast<int>(x_start), static_cast<int>(x_end));

    bool stroke_uniform = gpu_stroke.ok && (gpu_stroke.unique_nonzero_colors.size() == 1);
    bool stroke_continuous = gpu_stroke.ok && (gpu_stroke.spine_missing_pixels == 0);
    bool stroke_counts_match = gpu_stroke.ok && (gpu_stroke.nonzero_pixels == white_after_stroke) && (white_after_stroke > 0);

    // Test 2: Undo stroke clears m_mask_white_px AND GPU texture pixels back to 0
    bool undo_ok = tool.undo_stroke();
    auto u0 = std::chrono::high_resolution_clock::now();
    tool.update_gpu_texture(dev);
    auto u1 = std::chrono::high_resolution_clock::now();
    double undo_upload_ms = std::chrono::duration<double, std::milli>(u1 - u0).count();
    size_t white_after_undo = tool.get_mask_white_px();
    MaskGpuStats gpu_undo = readback_mask_gpu(dev, ctx, tool.get_mask_srv(), 0, 1, 0);
    bool undo_cleared = undo_ok && (white_after_undo == 0) && gpu_undo.ok && (gpu_undo.nonzero_pixels == 0);

    // Test 3: Redo restores, then Clear Mask genuinely zeroes everything
    bool redo_ok = tool.redo_stroke();
    tool.update_gpu_texture(dev);
    size_t white_after_redo = tool.get_mask_white_px();

    auto c0 = std::chrono::high_resolution_clock::now();
    tool.clear_mask();
    tool.update_gpu_texture(dev);
    auto c1 = std::chrono::high_resolution_clock::now();
    double clear_upload_ms = std::chrono::duration<double, std::milli>(c1 - c0).count();
    size_t white_after_clear = tool.get_mask_white_px();
    MaskGpuStats gpu_clear = readback_mask_gpu(dev, ctx, tool.get_mask_srv(), 0, 1, 0);
    bool clear_ok = redo_ok && (white_after_redo == white_after_stroke) &&
                    (white_after_clear == 0) && !tool.has_mask_edits() &&
                    gpu_clear.ok && (gpu_clear.nonzero_pixels == 0);

    bool latency_ok = (avg_step_ms < 1.0) && (max_step_ms < 1.0);
    bool all_passed = stroke_uniform && stroke_continuous && stroke_counts_match &&
                      undo_cleared && clear_ok && latency_ok;

    if (opts.json_output) {
        std::cout << "{\n"
                  << "  \"passed\": " << (all_passed ? "true" : "false") << ",\n"
                  << "  \"resolution\": \"" << W << "x" << H << "\",\n"
                  << "  \"avg_step_ms\": " << std::fixed << std::setprecision(4) << avg_step_ms << ",\n"
                  << "  \"max_step_ms\": " << max_step_ms << ",\n"
                  << "  \"undo_upload_ms\": " << undo_upload_ms << ",\n"
                  << "  \"clear_upload_ms\": " << clear_upload_ms << ",\n"
                  << "  \"latency_under_1ms\": " << (latency_ok ? "true" : "false") << ",\n"
                  << "  \"white_px_after_stroke\": " << white_after_stroke << ",\n"
                  << "  \"gpu_nonzero_after_stroke\": " << gpu_stroke.nonzero_pixels << ",\n"
                  << "  \"unique_nonzero_colors\": " << gpu_stroke.unique_nonzero_colors.size() << ",\n"
                  << "  \"spine_missing_pixels\": " << gpu_stroke.spine_missing_pixels << ",\n"
                  << "  \"undo_cleared\": " << (undo_cleared ? "true" : "false") << ",\n"
                  << "  \"clear_mask_cleared\": " << (clear_ok ? "true" : "false") << ",\n"
                  << "  \"white_px_after_clear\": " << white_after_clear << ",\n"
                  << "  \"gpu_nonzero_after_clear\": " << gpu_clear.nonzero_pixels << "\n"
                  << "}\n";
    } else {
        std::cout << "========================================\n"
                  << "  ShaderLab 4K Erase Mask & Perf Test\n"
                  << "========================================\n"
                  << "Resolution:               " << W << "x" << H << "\n"
                  << "Avg step (dab+GPU):       " << std::fixed << std::setprecision(4) << avg_step_ms << " ms\n"
                  << "Max step (dab+GPU):       " << max_step_ms << " ms  [" << (latency_ok ? "PASS <1ms" : "FAIL >=1ms") << "]\n"
                  << "Stroke CPU white px:      " << white_after_stroke << "\n"
                  << "Stroke GPU nonzero px:    " << gpu_stroke.nonzero_pixels << "  [" << (stroke_counts_match ? "PASS" : "FAIL") << "]\n"
                  << "Unique nonzero colors:    " << gpu_stroke.unique_nonzero_colors.size() << "  [" << (stroke_uniform ? "PASS uniform" : "FAIL ribbing") << "]\n"
                  << "Spine missing px:         " << gpu_stroke.spine_missing_pixels << "  [" << (stroke_continuous ? "PASS continuous" : "FAIL dots") << "]\n"
                  << "Undo cleared CPU/GPU:     " << white_after_undo << "/" << gpu_undo.nonzero_pixels
                  << " (" << undo_upload_ms << " ms) [" << (undo_cleared ? "PASS" : "FAIL") << "]\n"
                  << "Clear Mask CPU/GPU:       " << white_after_clear << "/" << gpu_clear.nonzero_pixels
                  << " (" << clear_upload_ms << " ms) [" << (clear_ok ? "PASS" : "FAIL") << "]\n"
                  << "OVERALL:                  " << (all_passed ? "PASS" : "FAIL") << "\n";
    }

    tool.clear_mask();
    gfx.shutdown();
    return all_passed ? 0 : 1;
}
