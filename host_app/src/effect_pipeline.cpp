#include "effect_pipeline.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace {

struct alignas(16) BuiltinEffectParams {
    float p0 = 0.0f;
    float p1 = 0.0f;
    float p2 = 0.0f;
    float p3 = 0.0f;
    float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float dims[4] = { 1920.0f, 1080.0f, 1.0f / 1920.0f, 1.0f / 1080.0f };
};

const char *kBuiltinVS = R"HLSL(
void VSMain(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv.x = (id == 2) ? 2.0 : 0.0;
    uv.y = (id == 1) ? 2.0 : 0.0;
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)HLSL";

const char *kBuiltinPassthroughPS = R"HLSL(
Texture2D g_Tex : register(t0);
SamplerState g_Smp : register(s0);

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return g_Tex.Sample(g_Smp, uv);
}
)HLSL";

const char *kBuiltinSharpenPS = R"HLSL(
cbuffer Params : register(b0)
{
    float p0; // curve_height
    float p1; // video_level_out
    float p2;
    float p3;
    float4 color;
    float4 dims; // xy=(w,h), zw=(1/w,1/h)
};
Texture2D g_Tex : register(t0);
SamplerState g_Smp : register(s0);

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 c = g_Tex.Sample(g_Smp, uv);
    float2 px = dims.zw;
    float3 n = g_Tex.Sample(g_Smp, uv + float2(0.0, -px.y)).rgb;
    float3 s = g_Tex.Sample(g_Smp, uv + float2(0.0,  px.y)).rgb;
    float3 e = g_Tex.Sample(g_Smp, uv + float2( px.x, 0.0)).rgb;
    float3 w = g_Tex.Sample(g_Smp, uv + float2(-px.x, 0.0)).rgb;

    float3 blur = (n + s + e + w) * 0.25;
    float3 detail = c.rgb - blur;
    float luma_c = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
    float luma_b = dot(blur, float3(0.2126, 0.7152, 0.0722));
    float edge_weight = saturate(1.0 - abs(luma_c - luma_b) * 4.0);
    float3 out_rgb = saturate(c.rgb + detail * (p0 * 1.45 * (0.4 + 0.6 * edge_weight)));
    if (p1 > 0.5)
    {
        out_rgb = saturate(out_rgb * (219.0 / 255.0) + (16.0 / 255.0));
    }
    return float4(out_rgb, c.a);
}
)HLSL";

const char *kBuiltinVignettePS = R"HLSL(
cbuffer Params : register(b0)
{
    float p0; // _VignetteIntensity
    float p1; // chromatic strength
    float p2;
    float p3;
    float4 color; // _VignetteColor
    float4 dims;
};
Texture2D g_Tex : register(t0);
SamplerState g_Smp : register(s0);

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 center = g_Tex.Sample(g_Smp, uv);
    float2 d = uv - 0.5;
    float r2 = dot(d, d);
    float ca_shift = p0 * 0.004;
    float r = g_Tex.Sample(g_Smp, uv - d * ca_shift).r;
    float g = center.g;
    float b = g_Tex.Sample(g_Smp, uv + d * ca_shift).b;
    float3 rgb = float3(r, g, b);

    float vig = saturate(1.0 - r2 * (1.2 + p0 * 1.8));
    rgb = lerp(color.rgb * rgb, rgb, vig);
    return float4(saturate(rgb), center.a);
}
)HLSL";

const char *kBuiltinFilmGrainPS = R"HLSL(
cbuffer Params : register(b0)
{
    float p0; // grain_amount
    float p1; // color_temperature (-1..1 or 6500K centered)
    float p2; // timer_sec
    float p3;
    float4 color;
    float4 dims;
};
Texture2D g_Tex : register(t0);
SamplerState g_Smp : register(s0);

float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 c = g_Tex.Sample(g_Smp, uv);
    float2 px_coord = floor(uv * dims.xy);
    float n = hash12(px_coord + frac(p2 * 13.37) * 512.0) - 0.5;

    float temp = clamp(p1, -1.0, 1.0);
    float3 grade = c.rgb * float3(1.0 + temp * 0.14, 1.0, 1.0 - temp * 0.14);
    float3 out_rgb = saturate(grade + n * (p0 * 0.25));
    return float4(out_rgb, c.a);
}
)HLSL";

bool compile_builtin_shader(
    const char *src,
    const char *entry,
    const char *target,
    ComPtr<ID3DBlob> &out_blob)
{
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(
        src,
        std::strlen(src),
        "BuiltinEffectPipeline",
        nullptr,
        nullptr,
        entry,
        target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &out_blob,
        &err
    );
    return SUCCEEDED(hr);
}

std::string normalize_key(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

float get_uniform_float(const ShaderLabEffect &eff, const char *name, float fallback) {
    for (const auto &u : eff.uniforms) {
        if (u.name == name) {
            if (u.type == ShaderLabUniform::Type::Bool) return u.bool_val ? 1.0f : 0.0f;
            if (u.type == ShaderLabUniform::Type::Int) return static_cast<float>(u.int_val);
            if (u.type == ShaderLabUniform::Type::Enum) return static_cast<float>(u.enum_selected);
            return u.float_val;
        }
    }
    return fallback;
}

} // namespace

EffectPipeline::EffectPipeline() = default;
EffectPipeline::~EffectPipeline() {
    shutdown();
}

bool EffectPipeline::initialize(ID3D11Device *device) {
    if (!device) return false;
    m_device = device;
    if (!compile_builtin_shaders(device)) return false;
    if (!create_states_and_fallbacks(device)) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(BuiltinEffectParams);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cbd, nullptr, &m_param_cbuffer))) {
        return false;
    }

    m_initialized = true;
    return true;
}

void EffectPipeline::shutdown() {
    m_runtimes.clear();
    m_failed_compile_keys.clear();
    m_tex_a.Reset(); m_rtv_a.Reset(); m_srv_a.Reset();
    m_tex_b.Reset(); m_rtv_b.Reset(); m_srv_b.Reset();
    m_ps_sharpen.Reset(); m_ps_vignette.Reset(); m_ps_filmgrain.Reset(); m_ps_passthrough.Reset();
    m_vs.Reset();
    m_sampler_linear.Reset(); m_sampler_point.Reset();
    m_sampler_linear_wrap.Reset(); m_sampler_point_wrap.Reset();
    m_sampler_linear_mirror.Reset(); m_sampler_linear_border.Reset();
    m_param_cbuffer.Reset();
    m_blend_all.Reset(); m_blend_rgb_only.Reset();
    m_raster_state.Reset();
    m_dummy_white_srv.Reset();
    m_device.Reset();
    m_target_w = 0;
    m_target_h = 0;
    m_initialized = false;
}

bool EffectPipeline::compile_builtin_shaders(ID3D11Device *device) {
    ComPtr<ID3DBlob> vs_blob, ps_pt, ps_sh, ps_vg, ps_fg;
    if (!compile_builtin_shader(kBuiltinVS, "VSMain", "vs_5_0", vs_blob)) return false;
    if (!compile_builtin_shader(kBuiltinPassthroughPS, "PSMain", "ps_5_0", ps_pt)) return false;
    if (!compile_builtin_shader(kBuiltinSharpenPS, "PSMain", "ps_5_0", ps_sh)) return false;
    if (!compile_builtin_shader(kBuiltinVignettePS, "PSMain", "ps_5_0", ps_vg)) return false;
    if (!compile_builtin_shader(kBuiltinFilmGrainPS, "PSMain", "ps_5_0", ps_fg)) return false;

    if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vs))) return false;
    if (FAILED(device->CreatePixelShader(ps_pt->GetBufferPointer(), ps_pt->GetBufferSize(), nullptr, &m_ps_passthrough))) return false;
    if (FAILED(device->CreatePixelShader(ps_sh->GetBufferPointer(), ps_sh->GetBufferSize(), nullptr, &m_ps_sharpen))) return false;
    if (FAILED(device->CreatePixelShader(ps_vg->GetBufferPointer(), ps_vg->GetBufferSize(), nullptr, &m_ps_vignette))) return false;
    if (FAILED(device->CreatePixelShader(ps_fg->GetBufferPointer(), ps_fg->GetBufferSize(), nullptr, &m_ps_filmgrain))) return false;
    return true;
}

bool EffectPipeline::create_states_and_fallbacks(ID3D11Device *device) {
    auto mk_sampler = [&](D3D11_FILTER f, D3D11_TEXTURE_ADDRESS_MODE addr, ComPtr<ID3D11SamplerState> &out) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = f;
        sd.AddressU = addr;
        sd.AddressV = addr;
        sd.AddressW = addr;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        return SUCCEEDED(device->CreateSamplerState(&sd, &out));
    };

    if (!mk_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, m_sampler_linear)) return false;
    if (!mk_sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, m_sampler_point)) return false;
    if (!mk_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP, m_sampler_linear_wrap)) return false;
    if (!mk_sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_WRAP, m_sampler_point_wrap)) return false;
    if (!mk_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_MIRROR, m_sampler_linear_mirror)) return false;
    if (!mk_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_BORDER, m_sampler_linear_border)) return false;

    D3D11_BLEND_DESC bd_all{};
    bd_all.RenderTarget[0].BlendEnable = FALSE;
    bd_all.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd_all, &m_blend_all))) return false;

    D3D11_BLEND_DESC bd_rgb{};
    bd_rgb.RenderTarget[0].BlendEnable = FALSE;
    bd_rgb.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(device->CreateBlendState(&bd_rgb, &m_blend_rgb_only))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    if (FAILED(device->CreateRasterizerState(&rd, &m_raster_state))) return false;

    // 1x1 white fallback texture for optional Depth SRVs when depth hasn't been generated yet
    uint32_t white_px = 0xFFFFFFFFu;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1;
    td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init_data{};
    init_data.pSysMem = &white_px;
    init_data.SysMemPitch = sizeof(uint32_t);
    ComPtr<ID3D11Texture2D> white_tex;
    if (FAILED(device->CreateTexture2D(&td, &init_data, &white_tex))) return false;
    if (FAILED(device->CreateShaderResourceView(white_tex.Get(), nullptr, &m_dummy_white_srv))) return false;

    return true;
}

bool EffectPipeline::ensure_targets(ID3D11Device *device, uint32_t width, uint32_t height) {
    if (!device || width == 0 || height == 0) return false;
    if (m_target_w == width && m_target_h == height && m_tex_a && m_tex_b) {
        return true;
    }

    m_tex_a.Reset(); m_rtv_a.Reset(); m_srv_a.Reset();
    m_tex_b.Reset(); m_rtv_b.Reset(); m_srv_b.Reset();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(device->CreateTexture2D(&td, nullptr, &m_tex_a))) return false;
    if (FAILED(device->CreateRenderTargetView(m_tex_a.Get(), nullptr, &m_rtv_a))) return false;
    if (FAILED(device->CreateShaderResourceView(m_tex_a.Get(), nullptr, &m_srv_a))) return false;

    if (FAILED(device->CreateTexture2D(&td, nullptr, &m_tex_b))) return false;
    if (FAILED(device->CreateRenderTargetView(m_tex_b.Get(), nullptr, &m_rtv_b))) return false;
    if (FAILED(device->CreateShaderResourceView(m_tex_b.Get(), nullptr, &m_srv_b))) return false;

    m_target_w = width;
    m_target_h = height;
    return true;
}

std::string EffectPipeline::make_runtime_key(const ShaderLabEffect &effect) {
    if (!effect.file_path.empty()) return normalize_key(effect.file_path);
    return normalize_key(effect.name);
}

bool EffectPipeline::load_and_compile_shader(
    ID3D11Device *device,
    const std::filesystem::path &path,
    uint32_t width,
    uint32_t height,
    ShaderLabEffect &out_effect,
    std::string &out_error)
{
    ID3D11Device *dev = device ? device : m_device.Get();
    if (!dev) {
        out_error = "D3D11 device not initialized";
        return false;
    }

    auto rt = std::make_unique<shaderlab::CompiledEffectRuntime>();
    if (!shaderlab::UniversalHlslParser::parse_and_compile_shader_file(
            dev,
            path,
            width > 0 ? width : 1920u,
            height > 0 ? height : 1080u,
            out_effect,
            *rt,
            out_error)) {
        return false;
    }

    std::string key = make_runtime_key(out_effect);
    m_failed_compile_keys.erase(key);
    m_runtimes[key] = std::move(rt);
    return true;
}

bool EffectPipeline::has_runtime(const std::string &effect_key) const {
    return m_runtimes.find(normalize_key(effect_key)) != m_runtimes.end();
}

void EffectPipeline::remove_runtime(const std::string &effect_key) {
    std::string k = normalize_key(effect_key);
    m_runtimes.erase(k);
    m_failed_compile_keys.erase(k);
}

void EffectPipeline::clear_external_runtimes() {
    m_runtimes.clear();
    m_failed_compile_keys.clear();
}

ID3D11SamplerState *EffectPipeline::select_sampler(int filter_mode, int address_mode) const {
    if (filter_mode == 1) {
        return (address_mode == 1) ? m_sampler_point_wrap.Get() : m_sampler_point.Get();
    }
    if (address_mode == 1) return m_sampler_linear_wrap.Get();
    if (address_mode == 2) return m_sampler_linear_mirror.Get();
    if (address_mode == 3) return m_sampler_linear_border.Get();
    return m_sampler_linear.Get();
}

void EffectPipeline::blit_passthrough(
    ID3D11DeviceContext *context,
    ID3D11ShaderResourceView *src_srv,
    ID3D11RenderTargetView *dst_rtv,
    uint32_t width,
    uint32_t height)
{
    ID3D11ShaderResourceView *null_srvs[16] = {};
    context->PSSetShaderResources(0, 16, null_srvs);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);
    context->RSSetState(m_raster_state.Get());
    context->OMSetRenderTargets(1, &dst_rtv, nullptr);
    context->OMSetBlendState(m_blend_all.Get(), nullptr, 0xFFFFFFFF);

    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps_passthrough.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &src_srv);
    ID3D11SamplerState *smp = m_sampler_point.Get();
    context->PSSetSamplers(0, 1, &smp);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->Draw(3, 0);
    context->PSSetShaderResources(0, 1, null_srvs);
}

void EffectPipeline::execute_runtime_effect(
    ID3D11DeviceContext *context,
    const ShaderLabEffect &effect,
    shaderlab::CompiledEffectRuntime &runtime,
    ID3D11ShaderResourceView *&current_srv,
    bool &use_a,
    uint32_t width,
    uint32_t height,
    ID3D11ShaderResourceView *depth_srv,
    float frametime_sec)
{
    runtime.ensure_custom_rts(m_device.Get(), width, height);

    for (const auto &pass : runtime.passes) {
        if (!pass.vertex_shader || !pass.pixel_shader) continue;

        ID3D11ShaderResourceView *null_srvs[16] = {};
        context->PSSetShaderResources(0, 16, null_srvs);

        ID3D11RenderTargetView *out_rtvs[8] = {};
        UINT rtv_count = 0;
        uint32_t vp_w = width;
        uint32_t vp_h = height;

        if (!pass.writes_to_main && !pass.output_rt_indices.empty()) {
            for (int rt_idx : pass.output_rt_indices) {
                if (rt_idx >= 0 && rt_idx < static_cast<int>(runtime.custom_rts.size()) && rtv_count < 8) {
                    const auto &rt = runtime.custom_rts[rt_idx];
                    out_rtvs[rtv_count++] = rt.rtv.Get();
                    vp_w = rt.cur_width;
                    vp_h = rt.cur_height;
                    if (pass.clear_rt && rt.rtv) {
                        static constexpr float kZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                        context->ClearRenderTargetView(rt.rtv.Get(), kZero);
                    }
                }
            }
        }

        bool writing_main = (rtv_count == 0);
        if (writing_main) {
            ID3D11RenderTargetView *dest_rtv = use_a ? m_rtv_a.Get() : m_rtv_b.Get();
            // Preserve true alpha when PS omits SV_Target alpha OR when running ReShade FX
            // that conventionally outputs opaque 1.0 alpha on BackBuffer shaders
            bool preserve_alpha = !pass.ps_outputs_alpha || (runtime.dialect == shaderlab::ShaderDialect::ReShadeFx);
            if (preserve_alpha) {
                blit_passthrough(context, current_srv, dest_rtv, width, height);
                context->OMSetBlendState(m_blend_rgb_only.Get(), nullptr, 0xFFFFFFFF);
            } else {
                context->OMSetBlendState(m_blend_all.Get(), nullptr, 0xFFFFFFFF);
            }
            out_rtvs[0] = dest_rtv;
            rtv_count = 1;
            vp_w = width;
            vp_h = height;
        } else {
            context->OMSetBlendState(m_blend_all.Get(), nullptr, 0xFFFFFFFF);
        }

        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(vp_w);
        vp.Height = static_cast<float>(vp_h);
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);
        context->RSSetState(m_raster_state.Get());
        context->OMSetRenderTargets(rtv_count, out_rtvs, nullptr);

        // Pack all reflected cbuffers
        const float time_sec = m_elapsed_timer_ms * 0.001f;
        const float ft_ms = frametime_sec * 1000.0f;
        for (const auto &cb : pass.cbuffers) {
            if (!cb.buffer || cb.byte_size == 0) continue;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(cb.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                uint8_t *dst = static_cast<uint8_t *>(mapped.pData);
                std::memset(dst, 0, cb.byte_size);

                for (const auto &b : cb.bindings) {
                    if (b.byte_offset >= cb.byte_size) continue;
                    size_t avail = cb.byte_size - b.byte_offset;

                    auto write_floats = [&](const float *src_f, size_t count) {
                        size_t bytes = std::min(avail, std::min(static_cast<size_t>(b.byte_size), count * sizeof(float)));
                        std::memcpy(dst + b.byte_offset, src_f, bytes);
                    };

                    if (b.var_name == "__sl_system_viewport") {
                        float v[4] = {
                            static_cast<float>(width),
                            static_cast<float>(height),
                            1.0f / std::max(1u, width),
                            1.0f / std::max(1u, height)
                        };
                        write_floats(v, 4);
                        continue;
                    }
                    if (b.var_name == "__sl_system_time") {
                        float v[4] = { ft_ms, m_elapsed_timer_ms, static_cast<float>(m_frame_count), time_sec };
                        write_floats(v, 4);
                        continue;
                    }
                    if (b.var_name == "__sl_framecount_i") {
                        int fc = static_cast<int>(m_frame_count);
                        size_t bytes = std::min(avail, sizeof(int));
                        std::memcpy(dst + b.byte_offset, &fc, bytes);
                        continue;
                    }

                    using Sem = shaderlab::SystemUniformSemantic;
                    if (b.system_semantic != Sem::None) {
                        switch (b.system_semantic) {
                            case Sem::FrameTimeMs: { write_floats(&ft_ms, 1); break; }
                            case Sem::FrameTimeSec: { write_floats(&frametime_sec, 1); break; }
                            case Sem::FrameCount: {
                                if (b.is_int || b.is_uint) {
                                    int fc = static_cast<int>(m_frame_count);
                                    std::memcpy(dst + b.byte_offset, &fc, std::min(avail, sizeof(int)));
                                } else {
                                    float fcf = static_cast<float>(m_frame_count);
                                    write_floats(&fcf, 1);
                                }
                                break;
                            }
                            case Sem::TimerMs: { write_floats(&m_elapsed_timer_ms, 1); break; }
                            case Sem::TimerSec: { write_floats(&time_sec, 1); break; }
                            case Sem::BufferSize: {
                                float v[2] = { static_cast<float>(width), static_cast<float>(height) };
                                write_floats(v, 2);
                                break;
                            }
                            case Sem::PixelSize: {
                                float v[2] = { 1.0f / std::max(1u, width), 1.0f / std::max(1u, height) };
                                write_floats(v, 2);
                                break;
                            }
                            case Sem::BufferWidth: { float v = static_cast<float>(width); write_floats(&v, 1); break; }
                            case Sem::BufferHeight: { float v = static_cast<float>(height); write_floats(&v, 1); break; }
                            case Sem::BufferRcpWidth: { float v = 1.0f / std::max(1u, width); write_floats(&v, 1); break; }
                            case Sem::BufferRcpHeight: { float v = 1.0f / std::max(1u, height); write_floats(&v, 1); break; }
                            case Sem::UnityTime: {
                                float v[4] = { time_sec * 0.05f, time_sec, time_sec * 2.0f, time_sec * 3.0f };
                                write_floats(v, 4);
                                break;
                            }
                            case Sem::UnitySinTime: {
                                float v[4] = {
                                    std::sin(time_sec * 0.125f),
                                    std::sin(time_sec * 0.25f),
                                    std::sin(time_sec * 0.5f),
                                    std::sin(time_sec)
                                };
                                write_floats(v, 4);
                                break;
                            }
                            case Sem::UnityScreenParams: {
                                float v[4] = {
                                    static_cast<float>(width),
                                    static_cast<float>(height),
                                    1.0f + 1.0f / std::max(1u, width),
                                    1.0f + 1.0f / std::max(1u, height)
                                };
                                write_floats(v, 4);
                                break;
                            }
                            case Sem::TexelSize: {
                                float v[4] = {
                                    1.0f / std::max(1u, width),
                                    1.0f / std::max(1u, height),
                                    static_cast<float>(width),
                                    static_cast<float>(height)
                                };
                                write_floats(v, 4);
                                break;
                            }
                            default: break;
                        }
                        continue;
                    }

                    if (b.ui_uniform_index >= 0 && b.ui_uniform_index < static_cast<int>(effect.uniforms.size())) {
                        const auto &u = effect.uniforms[b.ui_uniform_index];
                        switch (u.type) {
                            case ShaderLabUniform::Type::Float: {
                                float v[4] = { u.float_val, u.float_val, u.float_val, u.float_val };
                                write_floats(v, 4);
                                break;
                            }
                            case ShaderLabUniform::Type::Int: {
                                if (b.is_int || b.is_uint || b.is_bool) {
                                    std::memcpy(dst + b.byte_offset, &u.int_val, std::min(avail, sizeof(int)));
                                } else {
                                    float f = static_cast<float>(u.int_val);
                                    write_floats(&f, 1);
                                }
                                break;
                            }
                            case ShaderLabUniform::Type::Enum: {
                                if (b.is_int || b.is_uint || b.is_bool) {
                                    std::memcpy(dst + b.byte_offset, &u.enum_selected, std::min(avail, sizeof(int)));
                                } else {
                                    float f = static_cast<float>(u.enum_selected);
                                    write_floats(&f, 1);
                                }
                                break;
                            }
                            case ShaderLabUniform::Type::Bool: {
                                if (b.is_bool || b.is_int || b.is_uint) {
                                    int iv = u.bool_val ? 1 : 0;
                                    std::memcpy(dst + b.byte_offset, &iv, std::min(avail, sizeof(int)));
                                } else {
                                    float fv = u.bool_val ? 1.0f : 0.0f;
                                    write_floats(&fv, 1);
                                }
                                break;
                            }
                            case ShaderLabUniform::Type::Color3: {
                                write_floats(u.color_val, 3);
                                break;
                            }
                            case ShaderLabUniform::Type::Color4: {
                                write_floats(u.color_val, 4);
                                break;
                            }
                        }
                    }
                }
                context->Unmap(cb.buffer.Get(), 0);
            }

            ID3D11Buffer *buf_ptr = cb.buffer.Get();
            context->PSSetConstantBuffers(cb.bind_point, 1, &buf_ptr);
            context->VSSetConstantBuffers(cb.bind_point, 1, &buf_ptr);
        }

        // Bind reflected texture slots
        for (const auto &ts : pass.texture_slots) {
            ID3D11ShaderResourceView *srv = current_srv;
            if (ts.kind == shaderlab::ReflectedTextureSlot::SourceKind::DepthBuffer) {
                srv = depth_srv ? depth_srv : m_dummy_white_srv.Get();
            } else if (ts.kind == shaderlab::ReflectedTextureSlot::SourceKind::CustomRenderTarget &&
                       ts.custom_rt_index >= 0 &&
                       ts.custom_rt_index < static_cast<int>(runtime.custom_rts.size())) {
                srv = runtime.custom_rts[ts.custom_rt_index].srv.Get();
            }
            context->PSSetShaderResources(ts.bind_point, 1, &srv);
        }

        // Bind reflected sampler slots
        for (const auto &ss : pass.sampler_slots) {
            ID3D11SamplerState *smp = select_sampler(ss.filter_mode, ss.address_u);
            context->PSSetSamplers(ss.bind_point, 1, &smp);
        }

        context->VSSetShader(pass.vertex_shader.Get(), nullptr, 0);
        context->PSSetShader(pass.pixel_shader.Get(), nullptr, 0);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->Draw(3, 0);

        context->PSSetShaderResources(0, 16, null_srvs);

        if (writing_main) {
            current_srv = use_a ? m_srv_a.Get() : m_srv_b.Get();
            use_a = !use_a;
        }
    }

    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

ID3D11ShaderResourceView *EffectPipeline::process(
    ID3D11DeviceContext *context,
    ID3D11ShaderResourceView *input_srv,
    uint32_t width,
    uint32_t height,
    const std::vector<ShaderLabEffect> &effects,
    ID3D11ShaderResourceView *depth_srv,
    float frametime_sec)
{
    if (!m_initialized || !context || !input_srv || width == 0 || height == 0) {
        return input_srv;
    }

    m_frame_count++;
    m_elapsed_timer_ms += std::max(0.0f, frametime_sec) * 1000.0f;

    bool any_enabled = false;
    for (const auto &eff : effects) {
        if (eff.enabled) { any_enabled = true; break; }
    }
    if (!any_enabled) {
        return input_srv;
    }

    if (!ensure_targets(m_device.Get(), width, height)) {
        return input_srv;
    }

    ID3D11ShaderResourceView *current_srv = input_srv;
    bool use_a = true;

    for (const auto &eff : effects) {
        if (!eff.enabled) continue;

        // 1) Check dynamic runtime first
        std::string rkey = make_runtime_key(eff);
        auto rt_it = m_runtimes.find(rkey);
        if (rt_it == m_runtimes.end() && !eff.file_path.empty() &&
            eff.file_path.rfind("builtin", 0) != 0 &&
            m_failed_compile_keys.find(rkey) == m_failed_compile_keys.end())
        {
            std::error_code ec;
            std::filesystem::path fpath(eff.file_path);
            if (std::filesystem::exists(fpath, ec)) {
                auto new_rt = std::make_unique<shaderlab::CompiledEffectRuntime>();
                ShaderLabEffect temp_eff;
                std::string err;
                if (shaderlab::UniversalHlslParser::parse_and_compile_shader_file(
                        m_device.Get(), fpath, width, height, temp_eff, *new_rt, err)) {
                    rt_it = m_runtimes.emplace(rkey, std::move(new_rt)).first;
                } else {
                    m_failed_compile_keys.insert(rkey);
                }
            }
        }

        if (rt_it != m_runtimes.end() && rt_it->second && rt_it->second->valid) {
            execute_runtime_effect(
                context,
                eff,
                *rt_it->second,
                current_srv,
                use_a,
                width,
                height,
                depth_srv,
                frametime_sec
            );
            continue;
        }

        // 2) Built-in sample effects (backward compatible & alpha preserving)
        ID3D11PixelShader *builtin_ps = nullptr;
        BuiltinEffectParams params{};
        params.dims[0] = static_cast<float>(width);
        params.dims[1] = static_cast<float>(height);
        params.dims[2] = 1.0f / static_cast<float>(width);
        params.dims[3] = 1.0f / static_cast<float>(height);

        std::string lname = normalize_key(eff.name);
        if (lname.find("sharpen") != std::string::npos) {
            builtin_ps = m_ps_sharpen.Get();
            params.p0 = get_uniform_float(eff, "curve_height", 0.7f);
            params.p1 = get_uniform_float(eff, "video_level_out", 0.0f);
        } else if (lname.find("vignette") != std::string::npos) {
            builtin_ps = m_ps_vignette.Get();
            params.p0 = get_uniform_float(eff, "_VignetteIntensity", 0.4f);
            for (const auto &u : eff.uniforms) {
                if (u.name == "_VignetteColor") {
                    std::memcpy(params.color, u.color_val, sizeof(params.color));
                    break;
                }
            }
        } else if (lname.find("grain") != std::string::npos) {
            builtin_ps = m_ps_filmgrain.Get();
            params.p0 = get_uniform_float(eff, "grain_amount", 0.15f);
            params.p1 = get_uniform_float(eff, "color_temperature", 0.0f);
            params.p2 = m_elapsed_timer_ms * 0.001f;
        } else {
            continue;
        }

        ID3D11ShaderResourceView *null_srvs[16] = {};
        context->PSSetShaderResources(0, 16, null_srvs);

        ID3D11RenderTargetView *dest_rtv = use_a ? m_rtv_a.Get() : m_rtv_b.Get();
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(width);
        vp.Height = static_cast<float>(height);
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);
        context->RSSetState(m_raster_state.Get());
        context->OMSetRenderTargets(1, &dest_rtv, nullptr);
        context->OMSetBlendState(m_blend_all.Get(), nullptr, 0xFFFFFFFF);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(m_param_cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, &params, sizeof(params));
            context->Unmap(m_param_cbuffer.Get(), 0);
        }

        context->VSSetShader(m_vs.Get(), nullptr, 0);
        context->PSSetShader(builtin_ps, nullptr, 0);
        ID3D11Buffer *cb = m_param_cbuffer.Get();
        context->PSSetConstantBuffers(0, 1, &cb);
        context->PSSetShaderResources(0, 1, &current_srv);
        ID3D11SamplerState *smp = m_sampler_linear.Get();
        context->PSSetSamplers(0, 1, &smp);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->Draw(3, 0);

        context->PSSetShaderResources(0, 1, null_srvs);
        current_srv = use_a ? m_srv_a.Get() : m_srv_b.Get();
        use_a = !use_a;
    }

    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    return current_srv;
}
