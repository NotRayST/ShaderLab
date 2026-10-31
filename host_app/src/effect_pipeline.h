#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <memory>
#include "custom_ui.h"

using Microsoft::WRL::ComPtr;

class EffectPipeline {
public:
    EffectPipeline();
    ~EffectPipeline();

    bool initialize(ID3D11Device *device);
    void shutdown();

    bool ensure_targets(ID3D11Device *device, uint32_t width, uint32_t height);

    ID3D11ShaderResourceView *process(
        ID3D11DeviceContext *context,
        ID3D11ShaderResourceView *input_srv,
        uint32_t width, uint32_t height,
        const std::vector<ShaderLabEffect> &effects
    );

private:
    bool compile_builtin_shaders(ID3D11Device *device);

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11SamplerState> m_sampler_linear;
    ComPtr<ID3D11SamplerState> m_sampler_point;
    ComPtr<ID3D11Buffer> m_param_cbuffer;

    uint32_t m_target_w = 0;
    uint32_t m_target_h = 0;
    ComPtr<ID3D11Texture2D> m_tex_a;
    ComPtr<ID3D11RenderTargetView> m_rtv_a;
    ComPtr<ID3D11ShaderResourceView> m_srv_a;

    ComPtr<ID3D11Texture2D> m_tex_b;
    ComPtr<ID3D11RenderTargetView> m_rtv_b;
    ComPtr<ID3D11ShaderResourceView> m_srv_b;

    ComPtr<ID3D11PixelShader> m_ps_sharpen;
    ComPtr<ID3D11PixelShader> m_ps_vignette;
    ComPtr<ID3D11PixelShader> m_ps_filmgrain;
    ComPtr<ID3D11PixelShader> m_ps_passthrough;

    bool m_initialized = false;
};
