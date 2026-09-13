#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>

using Microsoft::WRL::ComPtr;

class SettleDetector {
public:
    SettleDetector() = default;
    ~SettleDetector() = default;

    void reset();
    void release_resources();

    bool check_convergence(
        ID3D11Device *device,
        ID3D11DeviceContext *context,
        IDXGISwapChain1 *swap_chain,
        float &out_delta,
        bool effects_active = false
    );

    float get_last_delta() const { return m_last_delta; }
    float get_last_filtered_delta() const { return m_last_lag; }
    float get_last_max_tile_delta() const { return m_last_delta; }
    bool is_converged() const { return m_consecutive_converged >= 8; }

private:
    static constexpr uint32_t kGridW = 160;
    static constexpr uint32_t kGridH = 90;
    static constexpr size_t kSampleCount = kGridW * kGridH;
    static constexpr uint32_t kHistoryFrames = 48;
    static constexpr uint32_t kLagFrames = 45;

    ComPtr<ID3D11Texture2D> m_staging[2];
    uint32_t m_staging_width = 0;
    uint32_t m_staging_height = 0;
    uint32_t m_ring_index = 0;
    uint32_t m_frames_in_flight = 0;

    std::vector<uint32_t> m_history[kHistoryFrames];
    float m_last_delta = 1.0f;
    float m_last_lag = 1.0f;
    int m_consecutive_converged = 0;
};
