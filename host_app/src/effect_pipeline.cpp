#include "effect_pipeline.h"
#include <d3dcompiler.h>
#include <iostream>

struct PipelineParamsBuffer {
    float params_a[4];
    float params_b[4];
    float resolution[4];
};

static const char g_pipeline_vs_hlsl[] = R"(
struct VSOutput {
    float4 Pos : SV_Position;
    float2 UV  : TEXCOORD0;
};

VSOutput main(uint vertex_id : SV_VertexID) {
    VSOutput output;
    output.UV = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.Pos = float4(output.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
)";

static const char g_pipeline_effects_hlsl[] = R"(
Texture2D g_InputTex : register(t0);
SamplerState g_LinearSampler : register(s0);
SamplerState g_PointSampler  : register(s1);

cbuffer EffectParams : register(b1) {
    float4 g_ParamsA;
    float4 g_ParamsB;
    float4 g_Resolution;
};

struct VSOutput {
    float4 Pos : SV_Position;
    float2 UV  : TEXCOORD0;
};

float4 passthrough_ps(VSOutput input) : SV_Target {
    return g_InputTex.Sample(g_LinearSampler, input.UV);
}

float4 sharpen_ps(VSOutput input) : SV_Target {
    float2 uv = input.UV;
    float2 step = g_Resolution.zw;
    float strength = g_ParamsA.x;
    float video_levels = g_ParamsA.y;

    float4 center = g_InputTex.Sample(g_PointSampler, uv);
    float4 up     = g_InputTex.Sample(g_PointSampler, uv - float2(0.0, step.y));
    float4 down   = g_InputTex.Sample(g_PointSampler, uv + float2(0.0, step.y));
    float4 left   = g_InputTex.Sample(g_PointSampler, uv - float2(step.x, 0.0));
    float4 right  = g_InputTex.Sample(g_PointSampler, uv + float2(step.x, 0.0));

    float4 laplacian = (up + down + left + right) * 0.25f - center;
    float4 sharpened = center - laplacian * (strength * 1.5f);
    sharpened = saturate(sharpened);

    if (video_levels > 0.5f) {
        sharpened.rgb = (sharpened.rgb * (235.0f - 16.0f) + 16.0f) / 255.0f;
    }

    return float4(sharpened.rgb, center.a);
}

float4 vignette_ps(VSOutput input) : SV_Target {
    float2 uv = input.UV;
    float intensity = g_ParamsA.x;
    float4 tint_color = g_ParamsB;

    float2 to_center = uv - 0.5f;
    float dist = length(to_center);
    float fringe = dist * 0.006f * intensity;

    float r = g_InputTex.Sample(g_LinearSampler, uv - to_center * fringe).r;
    float g = g_InputTex.Sample(g_LinearSampler, uv).g;
    float b = g_InputTex.Sample(g_LinearSampler, uv + to_center * fringe).b;
    float a = g_InputTex.Sample(g_LinearSampler, uv).a;

    float3 col = float3(r, g, b);

    float radius = 0.75f;
    float softness = 0.45f;
    float v = smoothstep(radius, radius - softness, dist * 1.414f);
    float vig_factor = lerp(1.0f, v, intensity);

    col = lerp(tint_color.rgb, col, vig_factor);

    return float4(saturate(col), a);
}

float4 filmgrain_ps(VSOutput input) : SV_Target {
    float2 uv = input.UV;
    float grain_size = g_ParamsA.x;
    int profile = (int)g_ParamsA.y;

    float4 col = g_InputTex.Sample(g_LinearSampler, uv);

    float2 p = uv * g_Resolution.xy;
    float noise = frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
    float grain = (noise - 0.5f) * (grain_size * 1.5f);
    col.rgb += grain;

    if (profile == 1) {
        col.r *= 1.08f;
        col.b *= 0.92f;
    } else if (profile == 2) {
        col.r *= 0.94f;
        col.b *= 1.08f;
    } else if (profile == 3) {
        float lum = dot(col.rgb, float3(0.299f, 0.587f, 0.114f));
        col.rgb = float3(lum, lum, lum);
    }

    return float4(saturate(col.rgb), col.a);
}
)";

EffectPipeline::EffectPipeline() = default;
EffectPipeline::~EffectPipeline() {
    shutdown();
}

bool EffectPipeline::initialize(ID3D11Device *device) {
    if (m_initialized) return true;
    if (!device) return false;
    m_device = device;

    if (!compile_builtin_shaders(device)) {
        std::cerr << "[EffectPipeline] Failed to compile builtin shaders\n";
        return false;
    }

    D3D11_SAMPLER_DESC samp_desc = {};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&samp_desc, &m_sampler_linear);

    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    device->CreateSamplerState(&samp_desc, &m_sampler_point);

    D3D11_BUFFER_DESC cb_desc = {};
    cb_desc.ByteWidth = sizeof(PipelineParamsBuffer);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&cb_desc, nullptr, &m_param_cbuffer);

    m_initialized = true;
    return true;
}

void EffectPipeline::shutdown() {
    m_tex_a.Reset();
    m_rtv_a.Reset();
    m_srv_a.Reset();
    m_tex_b.Reset();
    m_rtv_b.Reset();
    m_srv_b.Reset();
    m_param_cbuffer.Reset();
    m_sampler_linear.Reset();
    m_sampler_point.Reset();
    m_ps_sharpen.Reset();
    m_ps_vignette.Reset();
    m_ps_filmgrain.Reset();
    m_ps_passthrough.Reset();
    m_vs.Reset();
    m_device.Reset();
    m_initialized = false;
    m_target_w = 0;
    m_target_h = 0;
}

bool EffectPipeline::compile_builtin_shaders(ID3D11Device *device) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> vs_blob, ps_blob, error_blob;

    HRESULT hr = D3DCompile(g_pipeline_vs_hlsl, sizeof(g_pipeline_vs_hlsl), nullptr, nullptr, nullptr, "main", "vs_5_0", flags, 0, &vs_blob, &error_blob);
    if (FAILED(hr)) return false;
    device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vs);

    auto compile_ps = [&](const char *entry, ComPtr<ID3D11PixelShader> &out_ps) -> bool {
        ps_blob.Reset();
        error_blob.Reset();
        HRESULT h = D3DCompile(g_pipeline_effects_hlsl, sizeof(g_pipeline_effects_hlsl), nullptr, nullptr, nullptr, entry, "ps_5_0", flags, 0, &ps_blob, &error_blob);
        if (FAILED(h)) {
            if (error_blob) {
                std::cerr << "[EffectPipeline] PS compile error in " << entry << ": " << static_cast<const char *>(error_blob->GetBufferPointer()) << "\n";
            }
            return false;
        }
        return SUCCEEDED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &out_ps));
    };

    if (!compile_ps("passthrough_ps", m_ps_passthrough)) return false;
    if (!compile_ps("sharpen_ps", m_ps_sharpen)) return false;
    if (!compile_ps("vignette_ps", m_ps_vignette)) return false;
    if (!compile_ps("filmgrain_ps", m_ps_filmgrain)) return false;

    return true;
}

bool EffectPipeline::ensure_targets(ID3D11Device *device, uint32_t width, uint32_t height) {
    if (m_tex_a && m_target_w == width && m_target_h == height) {
        return true;
    }
    if (width == 0 || height == 0 || !device) return false;

    m_tex_a.Reset();
    m_rtv_a.Reset();
    m_srv_a.Reset();
    m_tex_b.Reset();
    m_rtv_b.Reset();
    m_srv_b.Reset();

    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr1 = device->CreateTexture2D(&tex_desc, nullptr, &m_tex_a);
    HRESULT hr2 = device->CreateTexture2D(&tex_desc, nullptr, &m_tex_b);
    if (FAILED(hr1) || FAILED(hr2)) return false;

    device->CreateRenderTargetView(m_tex_a.Get(), nullptr, &m_rtv_a);
    device->CreateShaderResourceView(m_tex_a.Get(), nullptr, &m_srv_a);
    device->CreateRenderTargetView(m_tex_b.Get(), nullptr, &m_rtv_b);
    device->CreateShaderResourceView(m_tex_b.Get(), nullptr, &m_srv_b);

    m_target_w = width;
    m_target_h = height;
    return true;
}

ID3D11ShaderResourceView *EffectPipeline::process(
    ID3D11DeviceContext *context,
    ID3D11ShaderResourceView *input_srv,
    uint32_t width, uint32_t height,
    const std::vector<ShaderLabEffect> &effects)
{
    if (!context || !input_srv || width == 0 || height == 0) return input_srv;
    if (!ensure_targets(m_device.Get(), width, height)) return input_srv;

    bool any_enabled = false;
    for (const auto &fx : effects) {
        if (fx.enabled) {
            any_enabled = true;
            break;
        }
    }
    if (!any_enabled) {
        return input_srv;
    }

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);

    ID3D11SamplerState *samplers[] = { m_sampler_linear.Get(), m_sampler_point.Get() };
    context->PSSetSamplers(0, 2, samplers);

    {
        ID3D11RenderTargetView *rtv = m_rtv_a.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        ID3D11ShaderResourceView *srv = input_srv;
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetShader(m_ps_passthrough.Get(), nullptr, 0);
        context->Draw(3, 0);
    }

    ID3D11ShaderResourceView *current_srv = m_srv_a.Get();
    ID3D11RenderTargetView *current_rtv = m_rtv_b.Get();
    ID3D11ShaderResourceView *next_srv = m_srv_b.Get();
    ID3D11RenderTargetView *next_rtv = m_rtv_a.Get();

    for (const auto &fx : effects) {
        if (!fx.enabled) continue;

        ID3D11PixelShader *target_ps = nullptr;
        PipelineParamsBuffer p = {};
        p.resolution[0] = static_cast<float>(width);
        p.resolution[1] = static_cast<float>(height);
        p.resolution[2] = 1.0f / static_cast<float>(width);
        p.resolution[3] = 1.0f / static_cast<float>(height);

        if (fx.name.find("Adaptive") != std::string::npos || fx.name.find("Sharpen") != std::string::npos) {
            target_ps = m_ps_sharpen.Get();
            for (const auto &u : fx.uniforms) {
                if (u.name == "curve_height") p.params_a[0] = u.float_val;
                if (u.name == "video_level_out") p.params_a[1] = u.bool_val ? 1.0f : 0.0f;
            }
        } else if (fx.name.find("Vignette") != std::string::npos) {
            target_ps = m_ps_vignette.Get();
            for (const auto &u : fx.uniforms) {
                if (u.name == "_VignetteIntensity") p.params_a[0] = u.float_val;
                if (u.name == "_VignetteColor") {
                    p.params_b[0] = u.color_val[0];
                    p.params_b[1] = u.color_val[1];
                    p.params_b[2] = u.color_val[2];
                    p.params_b[3] = u.color_val[3];
                }
            }
        } else if (fx.name.find("Grain") != std::string::npos || fx.name.find("Film") != std::string::npos) {
            target_ps = m_ps_filmgrain.Get();
            for (const auto &u : fx.uniforms) {
                if (u.name == "grain_amount") p.params_a[0] = u.float_val;
                if (u.name == "color_temperature") p.params_a[1] = static_cast<float>(u.enum_selected);
            }
        }

        if (!target_ps) continue;

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context->Map(m_param_cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, &p, sizeof(PipelineParamsBuffer));
            context->Unmap(m_param_cbuffer.Get(), 0);
        }

        ID3D11Buffer *cbs[] = { m_param_cbuffer.Get() };
        context->PSSetConstantBuffers(1, 1, cbs);

        context->OMSetRenderTargets(1, &current_rtv, nullptr);
        context->PSSetShaderResources(0, 1, &current_srv);
        context->PSSetShader(target_ps, nullptr, 0);
        context->Draw(3, 0);

        ID3D11ShaderResourceView *null_srv = nullptr;
        context->PSSetShaderResources(0, 1, &null_srv);

        std::swap(current_srv, next_srv);
        std::swap(current_rtv, next_rtv);
    }

    ID3D11RenderTargetView *null_rtv = nullptr;
    context->OMSetRenderTargets(1, &null_rtv, nullptr);

    ID3D11ShaderResourceView *null_srvs[2] = { nullptr, nullptr };
    context->PSSetShaderResources(0, 2, null_srvs);

    return current_srv;
}

