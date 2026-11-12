#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "GUI/types.h"
#include "hlsl_parser.h"

using Microsoft::WRL::ComPtr;
using gui::ShaderLabEffect;
using gui::ShaderLabUniform;

class EffectPipeline {
public:
    EffectPipeline();
    ~EffectPipeline();

    bool initialize(ID3D11Device *device);
    void shutdown();

    bool ensure_targets(ID3D11Device *device, uint32_t width, uint32_t height);

    bool load_and_compile_shader(
        ID3D11Device *device,
        const std::filesystem::path &path,
        uint32_t width,
        uint32_t height,
        ShaderLabEffect &out_effect,
        std::string &out_error
    );

    bool has_runtime(const std::string &effect_key) const;
    void remove_runtime(const std::string &effect_key);
    void clear_external_runtimes();

    ID3D11ShaderResourceView *process(
        ID3D11DeviceContext *context,
        ID3D11ShaderResourceView *input_srv,
        uint32_t width,
        uint32_t height,
        const std::vector<ShaderLabEffect> &effects,
        ID3D11ShaderResourceView *depth_srv = nullptr,
        float frametime_sec = 1.0f / 60.0f
    );

private:
    bool compile_builtin_shaders(ID3D11Device *device);
    bool create_states_and_fallbacks(ID3D11Device *device);
    void blit_passthrough(
        ID3D11DeviceContext *context,
        ID3D11ShaderResourceView *src_srv,
        ID3D11RenderTargetView *dst_rtv,
        uint32_t width,
        uint32_t height
    );
    void execute_runtime_effect(
        ID3D11DeviceContext *context,
        const ShaderLabEffect &effect,
        shaderlab::CompiledEffectRuntime &runtime,
        ID3D11ShaderResourceView *&current_srv,
        bool &use_a,
        uint32_t width,
        uint32_t height,
        ID3D11ShaderResourceView *depth_srv,
        float frametime_sec
    );
    static std::string make_runtime_key(const ShaderLabEffect &effect);
    ID3D11SamplerState *select_sampler(int filter_mode, int address_mode) const;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11SamplerState> m_sampler_linear;
    ComPtr<ID3D11SamplerState> m_sampler_point;
    ComPtr<ID3D11SamplerState> m_sampler_linear_wrap;
    ComPtr<ID3D11SamplerState> m_sampler_point_wrap;
    ComPtr<ID3D11SamplerState> m_sampler_linear_mirror;
    ComPtr<ID3D11SamplerState> m_sampler_linear_border;
    ComPtr<ID3D11Buffer> m_param_cbuffer;
    ComPtr<ID3D11BlendState> m_blend_all;
    ComPtr<ID3D11BlendState> m_blend_rgb_only;
    ComPtr<ID3D11RasterizerState> m_raster_state;
    ComPtr<ID3D11ShaderResourceView> m_dummy_white_srv;

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

    std::unordered_map<std::string, std::unique_ptr<shaderlab::CompiledEffectRuntime>> m_runtimes;
    std::unordered_set<std::string> m_failed_compile_keys;

    uint32_t m_frame_count = 0;
    float m_elapsed_timer_ms = 0.0f;
    bool m_initialized = false;
};
