#include "settle_detector.h"
#include <cmath>
#include <algorithm>

static double compute_sample_diff(const uint32_t *a, const uint32_t *b, size_t count) {
    if (!a || !b || count == 0) return 0.0;
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t p1 = a[i], p2 = b[i];
        total += std::abs(static_cast<int>(p1 & 0xFF) - static_cast<int>(p2 & 0xFF));
        total += std::abs(static_cast<int>((p1 >> 8) & 0xFF) - static_cast<int>((p2 >> 8) & 0xFF));
        total += std::abs(static_cast<int>((p1 >> 16) & 0xFF) - static_cast<int>((p2 >> 16) & 0xFF));
    }
    return static_cast<double>(total) / (3.0 * 255.0 * count);
}

void SettleDetector::reset() {
    m_ring_index = 0;
    m_frames_in_flight = 0;
    m_consecutive_converged = 0;
    m_last_delta = 1.0f;
    m_last_lag = 1.0f;
}

void SettleDetector::release_resources() {
    m_staging[0].Reset();
    m_staging[1].Reset();
    m_staging_width = 0;
    m_staging_height = 0;
    for (auto &buf : m_history) buf.clear();
    reset();
}

bool SettleDetector::check_convergence(
    ID3D11Device *device,
    ID3D11DeviceContext *context,
    IDXGISwapChain1 *swap_chain,
    float &out_delta,
    bool effects_active
) {
    out_delta = 1.0f;
    if (!device || !context || !swap_chain) return false;

    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) || !back_buffer) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    back_buffer->GetDesc(&desc);

    // allocate double buffered staging textures when resolution changes
    if (!m_staging[0] || !m_staging[1] || m_staging_width != desc.Width || m_staging_height != desc.Height) {
        m_staging[0].Reset();
        m_staging[1].Reset();
        m_staging_width = desc.Width;
        m_staging_height = desc.Height;

        D3D11_TEXTURE2D_DESC sdesc = desc;
        sdesc.MipLevels = 1;
        sdesc.ArraySize = 1;
        sdesc.SampleDesc.Count = 1;
        sdesc.Usage = D3D11_USAGE_STAGING;
        sdesc.BindFlags = 0;
        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sdesc.MiscFlags = 0;

        for (int i = 0; i < 2; ++i) {
            if (FAILED(device->CreateTexture2D(&sdesc, nullptr, &m_staging[i]))) return false;
        }
        for (auto &buf : m_history) buf.assign(kSampleCount, 0);
        reset();
    }

    uint32_t write_slot = m_ring_index % 2;
    context->CopyResource(m_staging[write_slot].Get(), back_buffer.Get());
    m_ring_index++;
    m_frames_in_flight++;

    if (m_frames_in_flight < 2) return false;

    uint32_t read_slot = 1 - write_slot;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(m_staging[read_slot].Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

    size_t curr_slot = (m_frames_in_flight - 2) % kHistoryFrames;
    const uint8_t *base_ptr = static_cast<const uint8_t *>(mapped.pData);

    uint32_t step_x = (std::max)(1u, m_staging_width / kGridW);
    uint32_t step_y = (std::max)(1u, m_staging_height / kGridH);

    for (uint32_t gy = 0; gy < kGridH; ++gy) {
        uint32_t cy = (std::min)(gy * step_y, m_staging_height - 1);
        const uint32_t *row = reinterpret_cast<const uint32_t *>(base_ptr + cy * mapped.RowPitch);
        for (uint32_t gx = 0; gx < kGridW; ++gx) {
            uint32_t cx = (std::min)(gx * step_x, m_staging_width - 1);
            m_history[curr_slot][gy * kGridW + gx] = row[cx];
        }
    }
    context->Unmap(m_staging[read_slot].Get(), 0);

    if (m_frames_in_flight < 3) return false;

    size_t prev_slot = (curr_slot + kHistoryFrames - 1) % kHistoryFrames;
    double gd_seq = compute_sample_diff(m_history[curr_slot].data(), m_history[prev_slot].data(), kSampleCount);

    if (!effects_active) {
        m_last_delta = static_cast<float>(gd_seq);
        m_last_lag = m_last_delta;
        out_delta = m_last_delta;
        if (gd_seq < 0.0001) {
            if (++m_consecutive_converged >= 2) return true;
        } else {
            m_consecutive_converged = 0;
        }
        return false;
    }

    if (m_frames_in_flight < kLagFrames + 2) return false;

    size_t lag_slot = (curr_slot + kHistoryFrames - kLagFrames) % kHistoryFrames;
    double gd_lag = compute_sample_diff(m_history[curr_slot].data(), m_history[lag_slot].data(), kSampleCount);

    double net_motion = (std::max)(0.0, gd_lag - gd_seq);

    m_last_delta = static_cast<float>(net_motion);
    m_last_lag = static_cast<float>(gd_lag);
    out_delta = m_last_delta;

    if (net_motion < 0.0008 || gd_lag < 0.0012) {
        if (++m_consecutive_converged >= 8) return true;
    } else {
        m_consecutive_converged = 0;
    }

    return false;
}
