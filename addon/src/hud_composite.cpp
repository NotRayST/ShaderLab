#include "hud_composite.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

static const char g_hud_hlsl[] = R"(
Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

struct VSOutput {
    float4 Pos : SV_Position;
    float2 UV  : TEXCOORD0;
};

VSOutput vs(uint vertex_id : SV_VertexID) {
    VSOutput output;
    output.UV = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.Pos = float4(output.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 ps(VSOutput input) : SV_Target {
    return g_Texture.Sample(g_Sampler, input.UV);
}
)";

struct HudPipeline {
    ComPtr<ID3D11VertexShader>   vs;
    ComPtr<ID3D11PixelShader>    ps;
    ComPtr<ID3D11SamplerState>   sampler;
    ComPtr<ID3D11BlendState>     blend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth_stencil;
    ID3D11Device *device = nullptr;
};

HudPipeline g_pipeline;

bool ensure_pipeline(ID3D11Device *device, HudPipeline &p) {
    if (p.device == device && p.vs && p.ps && p.sampler && p.blend && p.raster && p.depth_stencil) {
        return true;
    }

    p = HudPipeline{};
    p.device = device;

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

    ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;
    if (FAILED(D3DCompile(g_hud_hlsl, sizeof(g_hud_hlsl) - 1, nullptr, nullptr, nullptr,
                          "vs", "vs_5_0", flags, 0, &vs_blob, &err_blob))) {
        return false;
    }
    if (FAILED(D3DCompile(g_hud_hlsl, sizeof(g_hud_hlsl) - 1, nullptr, nullptr, nullptr,
                          "ps", "ps_5_0", flags, 0, &ps_blob, &err_blob))) {
        return false;
    }

    if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &p.vs)) ||
        FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &p.ps))) {
        p = HudPipeline{};
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &p.sampler))) {
        p = HudPipeline{};
        return false;
    }

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &p.blend))) {
        p = HudPipeline{};
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = FALSE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &p.raster))) {
        p = HudPipeline{};
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dsd.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&dsd, &p.depth_stencil))) {
        p = HudPipeline{};
        return false;
    }

    return true;
}

} // namespace

void composite_hud(
    SharedControlBlock *block,
    reshade::api::effect_runtime *runtime,
    reshade::api::command_list *cmd_list,
    reshade::api::resource_view rtv)
{
    if (!block || !runtime || !cmd_list) return;
    if (block->export_state != ExportState::Idle) return;

    reshade::api::device *api_device = runtime->get_device();
    if (!api_device || api_device->get_api() != reshade::api::device_api::d3d11) return;

    ID3D11Device *device = reinterpret_cast<ID3D11Device *>(static_cast<uintptr_t>(api_device->get_native()));
    ID3D11DeviceContext *context = reinterpret_cast<ID3D11DeviceContext *>(static_cast<uintptr_t>(cmd_list->get_native()));
    ID3D11RenderTargetView *bb_rtv = reinterpret_cast<ID3D11RenderTargetView *>(static_cast<uintptr_t>(rtv.handle));

    if (!device || !context || !bb_rtv) return;
    if (!ensure_pipeline(device, g_pipeline)) return;

    ID3D11ShaderResourceView *layers[kHudLayers];
    bool any = false;
    for (uint32_t i = 0; i < kHudLayers; ++i) {
        layers[i] = reinterpret_cast<ID3D11ShaderResourceView *>(static_cast<uintptr_t>(block->hud_srv[i]));
        if (layers[i]) any = true;
    }
    if (!any) return;

    reshade::api::resource bb_res = api_device->get_resource_from_view(rtv);
    reshade::api::resource_desc desc = api_device->get_resource_desc(bb_res);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(desc.texture.width);
    vp.Height = static_cast<float>(desc.texture.height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    if (vp.Width <= 0.0f || vp.Height <= 0.0f) {
        uint32_t w = 0, h = 0;
        runtime->get_screenshot_width_and_height(&w, &h);
        vp.Width = static_cast<float>(w > 0 ? w : (block ? block->view_image_width : 1920));
        vp.Height = static_cast<float>(h > 0 ? h : (block ? block->view_image_height : 1080));
    }

    ID3D11RenderTargetView *rtvs[] = { bb_rtv };
    context->OMSetRenderTargets(1, rtvs, nullptr);
    context->RSSetViewports(1, &vp);
    context->RSSetState(g_pipeline.raster.Get());
    context->OMSetDepthStencilState(g_pipeline.depth_stencil.Get(), 0);

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_pipeline.vs.Get(), nullptr, 0);
    context->PSSetShader(g_pipeline.ps.Get(), nullptr, 0);
    ID3D11SamplerState *samplers[] = { g_pipeline.sampler.Get() };
    context->PSSetSamplers(0, 1, samplers);

    float bf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState(g_pipeline.blend.Get(), bf, 0xFFFFFFFF);

    for (uint32_t i = 0; i < kHudLayers; ++i) {
        if (!layers[i]) continue;
        ID3D11ShaderResourceView *srvs[] = { layers[i] };
        context->PSSetShaderResources(0, 1, srvs);
        context->Draw(3, 0);
    }

    ID3D11ShaderResourceView *null_srvs[] = { nullptr };
    context->PSSetShaderResources(0, 1, null_srvs);
    context->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
    context->OMSetDepthStencilState(nullptr, 0);
}

void on_hud_destroy_device(reshade::api::device *device) {
    (void)device;
    g_pipeline = HudPipeline{};
}
