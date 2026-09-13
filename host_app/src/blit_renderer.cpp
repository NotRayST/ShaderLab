#include "blit_renderer.h"
#include <d3dcompiler.h>
#include <iostream>
#include <cmath>

static const char g_blit_hlsl[] = R"(
Texture2D g_Texture : register(t0);
SamplerState g_PointSampler : register(s0);
SamplerState g_LinearSampler : register(s1);

cbuffer XformBuffer : register(b0) {
    float4 g_XformA; // cos_angle, sin_angle, zoom, is_transformed
    float4 g_XformB; // pan_x, pan_y, canvas_w, canvas_h
    float4 g_XformC; // img_w, img_h, use_linear, far_plane
    float4 g_XformD; // is_preview, unused, unused, unused
};

struct VSOutput {
    float4 Pos : SV_Position;
    float2 UV  : TEXCOORD0;
};

VSOutput blit_vs(uint vertex_id : SV_VertexID) {
    VSOutput output;
    output.UV = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.Pos = float4(output.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 blit_ps(VSOutput input) : SV_Target {
    if (g_XformA.w < 0.5f) {
        return g_Texture.Sample(g_PointSampler, input.UV);
    }

    float cos_a = g_XformA.x;
    float sin_a = g_XformA.y;
    float zoom = g_XformA.z;
    float2 pan = g_XformB.xy;
    float2 canvas_dims = g_XformB.zw;
    float2 img_dims = g_XformC.xy;

    if (img_dims.x <= 0.0f || img_dims.y <= 0.0f || canvas_dims.x <= 0.0f || canvas_dims.y <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // offset from canvas center, scaled by zoom
    float2 p = (input.UV - 0.5f) * canvas_dims;
    p = p / zoom;

    // rotate by -angle (inverse rotation to find source texel)
    float2 q = float2(p.x * cos_a + p.y * sin_a, -p.x * sin_a + p.y * cos_a);

    float2 img_center = img_dims * 0.5f;
    float2 tex_pixel = q + img_center + pan;
    float2 uv = tex_pixel / img_dims;

    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    if (g_XformC.z > 0.5f) {
        return g_Texture.Sample(g_LinearSampler, uv);
    } else {
        return g_Texture.Sample(g_PointSampler, uv);
    }
}

float4 blit_depth_ps(VSOutput input) : SV_TARGET {
    float F = (g_XformC.w > 0.0f) ? g_XformC.w : 1000.0f;
    const float C = 1000.0f; // reshade linearization far plane
    bool is_preview = (g_XformD.x > 0.5f);

    if (g_XformA.w < 0.5f) {
        float z = g_Texture.Sample(g_PointSampler, input.UV).r;
        float d = (z * F) / (1.0f + z * (F - 1.0f));
        if (is_preview) {
            float z_reshade = d / (C - d * (C - 1.0f));
            return float4(z_reshade, z_reshade, z_reshade, 1.0f);
        }
        return float4(d, d, d, 1.0f);
    }

    float cos_a = g_XformA.x;
    float sin_a = g_XformA.y;
    float zoom = g_XformA.z;
    float2 pan = g_XformB.xy;
    float2 canvas_dims = g_XformB.zw;
    float2 img_dims = g_XformC.xy;

    if (img_dims.x <= 0.0f || img_dims.y <= 0.0f || canvas_dims.x <= 0.0f || canvas_dims.y <= 0.0f) {
        if (is_preview) discard;
        return float4(1.0f, 1.0f, 1.0f, 1.0f); // background is far plane (1.0)
    }

    float2 p = (input.UV - 0.5f) * canvas_dims;
    p = p / zoom;
    float2 q = float2(p.x * cos_a + p.y * sin_a, -p.x * sin_a + p.y * cos_a);
    float2 img_center = img_dims * 0.5f;
    float2 tex_pixel = q + img_center + pan;
    float2 uv = tex_pixel / img_dims;

    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
        if (is_preview) discard;
        return float4(1.0f, 1.0f, 1.0f, 1.0f); // background is far plane (1.0)
    }

    float z = (g_XformC.z > 0.5f) ? g_Texture.Sample(g_LinearSampler, uv).r : g_Texture.Sample(g_PointSampler, uv).r;
    float d = (z * F) / (1.0f + z * (F - 1.0f));
    if (is_preview) {
        float z_reshade = d / (C - d * (C - 1.0f));
        return float4(z_reshade, z_reshade, z_reshade, 1.0f);
    }
    return float4(d, d, d, 1.0f);
}
)";

BlitRenderer::BlitRenderer() = default;

BlitRenderer::~BlitRenderer() {
    shutdown();
}

bool BlitRenderer::initialize(ID3D11Device *device) {
    if (!device) return false;

    UINT compile_flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compile_flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    compile_flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> error_blob;
    HRESULT hr = D3DCompile(
        g_blit_hlsl, sizeof(g_blit_hlsl),
        nullptr, nullptr, nullptr,
        "blit_vs", "vs_5_0",
        compile_flags, 0,
        &vs_blob, &error_blob
    );

    if (FAILED(hr)) {
        if (error_blob) {
            std::cerr << "[BlitRenderer] VS compile error: " << static_cast<const char *>(error_blob->GetBufferPointer()) << "\n";
        }
        return false;
    }

    hr = device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vs);
    if (FAILED(hr)) return false;

    ComPtr<ID3DBlob> ps_blob;
    error_blob.Reset();
    hr = D3DCompile(
        g_blit_hlsl, sizeof(g_blit_hlsl),
        nullptr, nullptr, nullptr,
        "blit_ps", "ps_5_0",
        compile_flags, 0,
        &ps_blob, &error_blob
    );

    if (FAILED(hr)) {
        if (error_blob) {
            std::cerr << "[BlitRenderer] PS compile error: " << static_cast<const char *>(error_blob->GetBufferPointer()) << "\n";
        }
        return false;
    }

    hr = device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_ps);
    if (FAILED(hr)) return false;

    ComPtr<ID3DBlob> depth_ps_blob;
    error_blob.Reset();
    hr = D3DCompile(
        g_blit_hlsl, sizeof(g_blit_hlsl),
        nullptr, nullptr, nullptr,
        "blit_depth_ps", "ps_5_0",
        compile_flags, 0,
        &depth_ps_blob, &error_blob
    );

    if (FAILED(hr)) {
        if (error_blob) {
            std::cerr << "[BlitRenderer] Depth PS compile error: " << static_cast<const char *>(error_blob->GetBufferPointer()) << "\n";
        }
        return false;
    }

    hr = device->CreatePixelShader(depth_ps_blob->GetBufferPointer(), depth_ps_blob->GetBufferSize(), nullptr, &m_depth_ps);
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC point_samp_desc = {};
    point_samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    point_samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    point_samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    point_samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    point_samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    point_samp_desc.MinLOD = 0;
    point_samp_desc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = device->CreateSamplerState(&point_samp_desc, &m_point_sampler);
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC linear_samp_desc = {};
    linear_samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    linear_samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    linear_samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    linear_samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    linear_samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    linear_samp_desc.MinLOD = 0;
    linear_samp_desc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = device->CreateSamplerState(&linear_samp_desc, &m_linear_sampler);
    if (FAILED(hr)) return false;

    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = device->CreateBlendState(&blend_desc, &m_blend_state);
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC cb_desc = {};
    cb_desc.ByteWidth = sizeof(ConstantBufferData);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = device->CreateBuffer(&cb_desc, nullptr, &m_cbuffer);
    return SUCCEEDED(hr);
}

void BlitRenderer::shutdown() {
    m_cbuffer.Reset();
    m_blend_state.Reset();
    m_linear_sampler.Reset();
    m_point_sampler.Reset();
    m_depth_ps.Reset();
    m_ps.Reset();
    m_vs.Reset();
}

void BlitRenderer::render(ID3D11DeviceContext *context, ID3D11ShaderResourceView *srv) {
    if (!context || !m_vs || !m_ps || !srv) return;

    if (m_cbuffer) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context->Map(m_cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto *cb = static_cast<ConstantBufferData *>(mapped.pData);
            cb->xform_a[0] = 1.0f;
            cb->xform_a[1] = 0.0f;
            cb->xform_a[2] = 1.0f;
            cb->xform_a[3] = 0.0f; // is_transformed = false
            cb->xform_c[3] = 1000.0f;
            context->Unmap(m_cbuffer.Get(), 0);
        }
        ID3D11Buffer *cbs[] = { m_cbuffer.Get() };
        context->PSSetConstantBuffers(0, 1, cbs);
    }

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps.Get(), nullptr, 0);

    ID3D11SamplerState *samplers[] = { m_point_sampler.Get(), m_linear_sampler.Get() };
    context->PSSetSamplers(0, 2, samplers);

    ID3D11ShaderResourceView *srvs[] = { srv };
    context->PSSetShaderResources(0, 1, srvs);

    context->Draw(3, 0);

    ID3D11ShaderResourceView *null_srv[] = { nullptr };
    context->PSSetShaderResources(0, 1, null_srv);
}

void BlitRenderer::render_depth(ID3D11DeviceContext *context, ID3D11ShaderResourceView *depth_srv, float far_plane) {
    if (!context || !m_vs || !m_depth_ps || !depth_srv) return;

    if (m_cbuffer) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context->Map(m_cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto *cb = static_cast<ConstantBufferData *>(mapped.pData);
            cb->xform_a[0] = 1.0f;
            cb->xform_a[1] = 0.0f;
            cb->xform_a[2] = 1.0f;
            cb->xform_a[3] = 0.0f; // is_transformed = false
            cb->xform_b[0] = 0.0f;
            cb->xform_b[1] = 0.0f;
            cb->xform_b[2] = 0.0f;
            cb->xform_b[3] = 0.0f;
            cb->xform_c[0] = 0.0f;
            cb->xform_c[1] = 0.0f;
            cb->xform_c[2] = 0.0f;
            cb->xform_c[3] = (far_plane > 1.0f) ? far_plane : 1000.0f;
            context->Unmap(m_cbuffer.Get(), 0);
        }
        ID3D11Buffer *cbs[] = { m_cbuffer.Get() };
        context->PSSetConstantBuffers(0, 1, cbs);
    }

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_depth_ps.Get(), nullptr, 0);

    ID3D11SamplerState *samplers[] = { m_point_sampler.Get(), m_linear_sampler.Get() };
    context->PSSetSamplers(0, 2, samplers);

    ID3D11ShaderResourceView *srvs[] = { depth_srv };
    context->PSSetShaderResources(0, 1, srvs);

    context->Draw(3, 0);

    ID3D11ShaderResourceView *null_srv[] = { nullptr };
    context->PSSetShaderResources(0, 1, null_srv);
}

void BlitRenderer::render_blend(ID3D11DeviceContext *context, ID3D11ShaderResourceView *srv) {
    if (!context || !m_vs || !m_ps || !srv) return;

    float blend_factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState(m_blend_state.Get(), blend_factor, 0xFFFFFFFF);

    render(context, srv);

    context->OMSetBlendState(nullptr, blend_factor, 0xFFFFFFFF);
}

void BlitRenderer::render_transformed(
    ID3D11DeviceContext *context,
    ID3D11ShaderResourceView *srv,
    const ViewportTransform &xform,
    float canvas_w,
    float canvas_h,
    float img_w,
    float img_h
) {
    if (!context || !m_vs || !m_ps || !srv || !m_cbuffer) return;

    float rad = xform.angle * (3.14159265358979323846f / 180.0f);
    float cos_a = std::cos(rad);
    float sin_a = std::sin(rad);

    float rot_w = std::fabs(cos_a * img_w) + std::fabs(sin_a * img_h);
    float rot_h = std::fabs(sin_a * img_w) + std::fabs(cos_a * img_h);
    rot_w = (std::max)(rot_w, 1.0f);
    rot_h = (std::max)(rot_h, 1.0f);
    float fit_scale = (std::min)(canvas_w / rot_w, canvas_h / rot_h);
    float eff_zoom = xform.zoom * fit_scale;

    float norm_deg = std::fmod(std::abs(xform.angle), 360.0f);
    bool is_ortho = (std::abs(norm_deg - 0.0f) < 0.01f ||
                     std::abs(norm_deg - 90.0f) < 0.01f ||
                     std::abs(norm_deg - 180.0f) < 0.01f ||
                     std::abs(norm_deg - 270.0f) < 0.01f);
    bool is_unzoomed = (std::abs(eff_zoom - 1.0f) < 0.001f);
    float use_linear = (is_ortho && is_unzoomed) ? 0.0f : 1.0f;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context->Map(m_cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto *cb = static_cast<ConstantBufferData *>(mapped.pData);
        cb->xform_a[0] = cos_a;
        cb->xform_a[1] = sin_a;
        cb->xform_a[2] = eff_zoom;
        cb->xform_a[3] = 1.0f; // is_transformed = true

        cb->xform_b[0] = xform.pan_x;
        cb->xform_b[1] = xform.pan_y;
        cb->xform_b[2] = canvas_w;
        cb->xform_b[3] = canvas_h;

        cb->xform_c[0] = img_w;
        cb->xform_c[1] = img_h;
        cb->xform_c[2] = use_linear;
        cb->xform_c[3] = 1000.0f;

        cb->xform_d[0] = 0.0f;
        cb->xform_d[1] = 0.0f;
        cb->xform_d[2] = 0.0f;
        cb->xform_d[3] = 0.0f;

        context->Unmap(m_cbuffer.Get(), 0);
    }

    ID3D11Buffer *cbs[] = { m_cbuffer.Get() };
    context->PSSetConstantBuffers(0, 1, cbs);

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps.Get(), nullptr, 0);

    ID3D11SamplerState *samplers[] = { m_point_sampler.Get(), m_linear_sampler.Get() };
    context->PSSetSamplers(0, 2, samplers);

    ID3D11ShaderResourceView *srvs[] = { srv };
    context->PSSetShaderResources(0, 1, srvs);

    context->Draw(3, 0);

    ID3D11ShaderResourceView *null_srv[] = { nullptr };
    context->PSSetShaderResources(0, 1, null_srv);
}

void BlitRenderer::render_depth_transformed(
    ID3D11DeviceContext *context,
    ID3D11ShaderResourceView *depth_srv,
    const ViewportTransform &xform,
    float canvas_w,
    float canvas_h,
    float img_w,
    float img_h,
    float far_plane,
    bool is_preview
) {
    if (!context || !m_vs || !m_depth_ps || !depth_srv || !m_cbuffer) return;

    float rad = xform.angle * (3.14159265358979323846f / 180.0f);
    float cos_a = std::cos(rad);
    float sin_a = std::sin(rad);

    float rot_w = std::fabs(cos_a * img_w) + std::fabs(sin_a * img_h);
    float rot_h = std::fabs(sin_a * img_w) + std::fabs(cos_a * img_h);
    rot_w = (std::max)(rot_w, 1.0f);
    rot_h = (std::max)(rot_h, 1.0f);
    float fit_scale = (std::min)(canvas_w / rot_w, canvas_h / rot_h);
    float eff_zoom = xform.zoom * fit_scale;

    float norm_deg = std::fmod(std::abs(xform.angle), 360.0f);
    bool is_ortho = (std::abs(norm_deg - 0.0f) < 0.01f ||
                     std::abs(norm_deg - 90.0f) < 0.01f ||
                     std::abs(norm_deg - 180.0f) < 0.01f ||
                     std::abs(norm_deg - 270.0f) < 0.01f);
    bool is_unzoomed = (std::abs(eff_zoom - 1.0f) < 0.001f);
    float use_linear = (is_ortho && is_unzoomed) ? 0.0f : 1.0f;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context->Map(m_cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto *cb = static_cast<ConstantBufferData *>(mapped.pData);
        cb->xform_a[0] = cos_a;
        cb->xform_a[1] = sin_a;
        cb->xform_a[2] = eff_zoom;
        cb->xform_a[3] = 1.0f; // is_transformed = true

        cb->xform_b[0] = xform.pan_x;
        cb->xform_b[1] = xform.pan_y;
        cb->xform_b[2] = canvas_w;
        cb->xform_b[3] = canvas_h;

        cb->xform_c[0] = img_w;
        cb->xform_c[1] = img_h;
        cb->xform_c[2] = use_linear;
        cb->xform_c[3] = (far_plane > 0.0f) ? far_plane : 1000.0f;

        cb->xform_d[0] = is_preview ? 1.0f : 0.0f;
        cb->xform_d[1] = 0.0f;
        cb->xform_d[2] = 0.0f;
        cb->xform_d[3] = 0.0f;

        context->Unmap(m_cbuffer.Get(), 0);
    }

    ID3D11Buffer *cbs[] = { m_cbuffer.Get() };
    context->PSSetConstantBuffers(0, 1, cbs);

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_depth_ps.Get(), nullptr, 0);

    ID3D11SamplerState *samplers[] = { m_point_sampler.Get(), m_linear_sampler.Get() };
    context->PSSetSamplers(0, 2, samplers);

    ID3D11ShaderResourceView *srvs[] = { depth_srv };
    context->PSSetShaderResources(0, 1, srvs);

    context->Draw(3, 0);

    ID3D11ShaderResourceView *null_srv[] = { nullptr };
    context->PSSetShaderResources(0, 1, null_srv);
}

void BlitRenderer::render_depth_preview(
    ID3D11DeviceContext *context,
    ID3D11ShaderResourceView *depth_srv,
    const ViewportTransform &xform,
    float canvas_w,
    float canvas_h,
    float img_w,
    float img_h,
    float far_plane
) {
    // renders depth preview matching reshade DisplayDepth.fx linearization
    render_depth_transformed(context, depth_srv, xform, canvas_w, canvas_h, img_w, img_h, far_plane, true);
}
