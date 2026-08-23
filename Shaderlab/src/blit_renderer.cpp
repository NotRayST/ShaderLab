#include "blit_renderer.h"
#include <d3dcompiler.h>
#include <iostream>

// inline copy of blit.hlsl so we dont need to ship a separate file for the compiled case
static const char g_blit_hlsl[] = R"(
Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

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
    return g_Texture.Sample(g_Sampler, input.UV);
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

    D3D11_SAMPLER_DESC samp_desc = {};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp_desc.MinLOD = 0;
    samp_desc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = device->CreateSamplerState(&samp_desc, &m_sampler);
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
    return SUCCEEDED(hr);
}

void BlitRenderer::shutdown() {
    m_blend_state.Reset();
    m_sampler.Reset();
    m_ps.Reset();
    m_vs.Reset();
}

void BlitRenderer::render(ID3D11DeviceContext *context, ID3D11ShaderResourceView *srv) {
    if (!context || !m_vs || !m_ps || !srv) return;

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    ID3D11ShaderResourceView *srvs[] = { srv };
    context->PSSetShaderResources(0, 1, srvs);

    context->Draw(3, 0);

    ID3D11ShaderResourceView *null_srv[] = { nullptr };
    context->PSSetShaderResources(0, 1, null_srv);
}

void BlitRenderer::render_blend(ID3D11DeviceContext *context, ID3D11ShaderResourceView *srv) {
    if (!context || !m_vs || !m_ps || !srv) return;

    float blend_factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState(m_blend_state.Get(), blend_factor, 0xFFFFFFFF);

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    ID3D11ShaderResourceView *srvs[] = { srv };
    context->PSSetShaderResources(0, 1, srvs);

    context->Draw(3, 0);

    ID3D11ShaderResourceView *null_srv[] = { nullptr };
    context->PSSetShaderResources(0, 1, null_srv);
    context->OMSetBlendState(nullptr, blend_factor, 0xFFFFFFFF);
}
