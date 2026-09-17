#ifndef ImTextureID
#define ImTextureID ImU64
#endif
#include <imgui.h>
#include "before_after.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace {

static const char g_ba_hlsl[] = R"(
Texture2D g_Before : register(t0);
Texture2D g_After  : register(t1);
SamplerState g_Sampler : register(s0);

cbuffer SplitCB : register(b0) {
    float2 g_Normal;
    float  g_SplitOffsetPx;
    float  g_LineHalfWPx;
    float2 g_Resolution;
    float2 g_Pad;
};

struct VSOutput { float4 Pos : SV_Position; float2 UV : TEXCOORD0; };

VSOutput vs(uint id : SV_VertexID) {
    VSOutput o;
    o.UV = float2((id << 1) & 2, id & 2);
    o.Pos = float4(o.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float4 ps(VSOutput i) : SV_Target {
    float2 pos = (i.UV - 0.5f) * g_Resolution;
    float d = dot(pos, g_Normal) - g_SplitOffsetPx;
    float4 col = (d < 0.0f) ? g_Before.Sample(g_Sampler, i.UV) : g_After.Sample(g_Sampler, i.UV);
    float ad = abs(d);
    float line_a = saturate(1.25f - ad);
    float shadow_a = saturate(2.25f - ad) * 0.55f;
    col = lerp(col, float4(0.0f, 0.0f, 0.0f, 1.0f), shadow_a);
    col = lerp(col, float4(1.0f, 1.0f, 1.0f, 1.0f), line_a);
    return col;
}
)";

struct alignas(16) SplitCBData {
    float normal_x, normal_y, split_offset_px, line_half_w_px;
    float res_w, res_h, pad0, pad1;
};

struct Pipeline {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader>  ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer>       cb;
    ID3D11Device *device = nullptr;
} g_pipe;

ComPtr<ID3D11Texture2D>          g_before_tex, g_after_tex;
ComPtr<ID3D11ShaderResourceView> g_before_srv, g_after_srv;
UINT        g_tex_w = 0, g_tex_h = 0;
DXGI_FORMAT g_tex_fmt = DXGI_FORMAT_UNKNOWN;
bool        g_before_valid = false;
BeforeAfterState s_state;

bool ensure_pipeline(ID3D11Device *dev) {
    if (g_pipe.device == dev && g_pipe.vs && g_pipe.ps && g_pipe.sampler && g_pipe.cb) return true;
    g_pipe = Pipeline{ nullptr, nullptr, nullptr, nullptr, dev };

    ComPtr<ID3DBlob> vs_blob, ps_blob;
    UINT f = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (FAILED(D3DCompile(g_ba_hlsl, sizeof(g_ba_hlsl) - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0", f, 0, &vs_blob, nullptr)) ||
        FAILED(D3DCompile(g_ba_hlsl, sizeof(g_ba_hlsl) - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", f, 0, &ps_blob, nullptr)))
        return false;

    if (FAILED(dev->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &g_pipe.vs)) ||
        FAILED(dev->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &g_pipe.ps)))
        return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &g_pipe.sampler))) return false;

    D3D11_BUFFER_DESC cbd = { sizeof(SplitCBData), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    return SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &g_pipe.cb));
}

bool ensure_textures(ID3D11Device *dev, ID3D11Resource *bb) {
    ComPtr<ID3D11Texture2D> bb2d;
    if (FAILED(bb->QueryInterface(IID_PPV_ARGS(&bb2d)))) return false;
    D3D11_TEXTURE2D_DESC td = {};
    bb2d->GetDesc(&td);

    if (g_before_tex && g_tex_w == td.Width && g_tex_h == td.Height && g_tex_fmt == td.Format) return true;

    g_before_tex.Reset(); g_before_srv.Reset();
    g_after_tex.Reset();  g_after_srv.Reset();

    td.MipLevels = 1; td.ArraySize = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0; td.MiscFlags = 0;

    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_before_tex)) ||
        FAILED(dev->CreateTexture2D(&td, nullptr, &g_after_tex)) ||
        FAILED(dev->CreateShaderResourceView(g_before_tex.Get(), nullptr, &g_before_srv)) ||
        FAILED(dev->CreateShaderResourceView(g_after_tex.Get(), nullptr, &g_after_srv)))
        return false;

    g_tex_w = td.Width; g_tex_h = td.Height; g_tex_fmt = td.Format;
    return true;
}

static bool get_d3d(reshade::api::effect_runtime *rt, reshade::api::command_list *cmd, reshade::api::resource_view rtv,
                    ID3D11Device *&dev, ID3D11DeviceContext *&ctx, ID3D11Resource *&res) {
    auto *ad = rt ? rt->get_device() : nullptr;
    if (!ad || ad->get_api() != reshade::api::device_api::d3d11) return false;
    dev = reinterpret_cast<ID3D11Device *>(static_cast<uintptr_t>(ad->get_native()));
    ctx = reinterpret_cast<ID3D11DeviceContext *>(static_cast<uintptr_t>(cmd->get_native()));
    auto r = ad->get_resource_from_view(rtv);
    res = r.handle ? reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(r.handle)) : nullptr;
    return dev && ctx && res;
}

} // namespace

BeforeAfterState &before_after_get_state() { return s_state; }

void before_after_toggle() {
    s_state.enabled = !s_state.enabled;
    if (!s_state.enabled) s_state.is_dragging_pos = s_state.is_dragging_rot = false;
}

bool before_after_handle_input(bool bg_hovered, bool any_active, bool fine) {
    if (!s_state.enabled) {
        s_state.is_dragging_pos = s_state.is_dragging_rot = false;
        return false;
    }

    ImGuiIO &io = ImGui::GetIO();
    float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (w <= 0.0f || h <= 0.0f) return false;

    float cx = w * 0.5f, cy = h * 0.5f;
    float px = io.MousePos.x - cx, py = io.MousePos.y - cy;
    float rad = s_state.angle * (3.1415926535f / 180.0f);
    float nx = std::cos(rad), ny = std::sin(rad);
    float extent = std::abs(w * nx) + std::abs(h * ny);
    float offset_px = s_state.split_offset * (extent > 0.0f ? extent : 1.0f);
    float dist = std::abs((px * nx + py * ny) - offset_px);

    bool near_divider = bg_hovered && (dist <= 10.0f);
    if (near_divider || s_state.is_dragging_pos) {
        SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
    }

    if (io.MouseDown[0] && !s_state.is_dragging_rot) {
        if (!s_state.is_dragging_pos && near_divider && !any_active && !io.KeyAlt) {
            s_state.is_dragging_pos = true;
        }
        if (s_state.is_dragging_pos) {
            float proj = px * nx + py * ny;
            s_state.split_offset = std::clamp(proj / (extent > 0.0f ? extent : 1.0f), -0.5f, 0.5f);
        }
    } else {
        s_state.is_dragging_pos = false;
    }

    static float s_last_drag_ang = 0.0f;
    static float s_raw_angle = 0.0f;
    bool rotate_btn = io.MouseDown[1] || (io.MouseDown[0] && io.KeyAlt);

    if (rotate_btn && !s_state.is_dragging_pos) {
        if (!s_state.is_dragging_rot && bg_hovered && !any_active) {
            s_state.is_dragging_rot = true;
            s_last_drag_ang = std::atan2(px, -py) * (180.0f / 3.14159265f);
            if (s_last_drag_ang < 0.0f) s_last_drag_ang += 360.0f;
            s_raw_angle = s_state.angle;
        }
        if (s_state.is_dragging_rot && (px * px + py * py > 16.0f)) {
            float cur_ang = std::atan2(px, -py) * (180.0f / 3.14159265f);
            if (cur_ang < 0.0f) cur_ang += 360.0f;
            float delta = cur_ang - s_last_drag_ang;
            if (delta > 180.0f) delta -= 360.0f;
            else if (delta < -180.0f) delta += 360.0f;
            s_last_drag_ang = cur_ang;
            if (fine) delta *= 0.25f;
            s_raw_angle += delta;
            s_raw_angle = std::fmod(s_raw_angle, 360.0f);
            if (s_raw_angle < 0.0f) s_raw_angle += 360.0f;

            float snapped = s_raw_angle;
            if (!fine) {
                constexpr float kSnap = 45.0f, kRadius = 4.0f;
                float r = std::fmod(s_raw_angle + 0.5f * kSnap, kSnap) - 0.5f * kSnap;
                if (std::abs(r) < kRadius) snapped = s_raw_angle - r;
            }
            s_state.angle = snapped;
        }
    } else {
        s_state.is_dragging_rot = false;
    }

    return s_state.is_dragging_pos || s_state.is_dragging_rot;
}

void before_after_capture_pre(SharedControlBlock *b, reshade::api::effect_runtime *rt,
                              reshade::api::command_list *cmd, reshade::api::resource_view rtv) {
    g_before_valid = false;
    if (b) {
        if (!s_state.is_dragging_pos && !s_state.is_dragging_rot) {
            s_state.enabled = (b->view_interaction_flags & VIEW_FLAG_BEFORE_AFTER) != 0;
            s_state.angle = b->before_after_angle;
            s_state.split_offset = b->before_after_split;
        } else {
            b->before_after_angle = s_state.angle;
            b->before_after_split = s_state.split_offset;
        }
    }
    if (!s_state.enabled || (b && b->export_state != ExportState::Idle)) return;
    ID3D11Device *dev; ID3D11DeviceContext *ctx; ID3D11Resource *bb;
    if (!get_d3d(rt, cmd, rtv, dev, ctx, bb) || !ensure_textures(dev, bb)) return;
    ctx->CopyResource(g_before_tex.Get(), bb);
    g_before_valid = true;
}

void before_after_composite(SharedControlBlock *b, reshade::api::effect_runtime *rt,
                            reshade::api::command_list *cmd, reshade::api::resource_view rtv) {
    if (b) {
        if (!s_state.is_dragging_pos && !s_state.is_dragging_rot) {
            s_state.enabled = (b->view_interaction_flags & VIEW_FLAG_BEFORE_AFTER) != 0;
            s_state.angle = b->before_after_angle;
            s_state.split_offset = b->before_after_split;
        } else {
            b->before_after_angle = s_state.angle;
            b->before_after_split = s_state.split_offset;
        }
    }
    if (!g_before_valid || !s_state.enabled || (b && b->export_state != ExportState::Idle)) return;
    g_before_valid = false;

    ID3D11Device *dev; ID3D11DeviceContext *ctx; ID3D11Resource *bb;
    if (!get_d3d(rt, cmd, rtv, dev, ctx, bb) || !ensure_pipeline(dev) || !g_before_srv || !g_after_srv) return;

    ctx->CopyResource(g_after_tex.Get(), bb);

    float rad = s_state.angle * (3.1415926535f / 180.0f);
    float nx = std::cos(rad), ny = std::sin(rad);
    float extent = std::abs((float)g_tex_w * nx) + std::abs((float)g_tex_h * ny);
    SplitCBData cb{ nx, ny, s_state.split_offset * extent, 1.25f, (float)g_tex_w, (float)g_tex_h, 0, 0 };
    ctx->UpdateSubresource(g_pipe.cb.Get(), 0, nullptr, &cb, 0, 0);

    D3D11_VIEWPORT vp{ 0.0f, 0.0f, (float)g_tex_w, (float)g_tex_h, 0.0f, 1.0f };
    ID3D11RenderTargetView *rtvs[] = { reinterpret_cast<ID3D11RenderTargetView *>(static_cast<uintptr_t>(rtv.handle)) };
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(nullptr);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_pipe.vs.Get(), nullptr, 0);
    ctx->PSSetShader(g_pipe.ps.Get(), nullptr, 0);

    ID3D11SamplerState *samplers[] = { g_pipe.sampler.Get() };
    ctx->PSSetSamplers(0, 1, samplers);
    ID3D11Buffer *cbs[] = { g_pipe.cb.Get() };
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView *srvs[] = { g_before_srv.Get(), g_after_srv.Get() };
    ctx->PSSetShaderResources(0, 2, srvs);

    ctx->Draw(3, 0);

    ID3D11ShaderResourceView *null_srvs[] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, null_srvs);
}

void on_before_after_destroy_device(reshade::api::device *) {
    g_pipe = Pipeline{};
    g_before_tex.Reset(); g_before_srv.Reset();
    g_after_tex.Reset();  g_after_srv.Reset();
    g_tex_w = g_tex_h = 0;
    g_tex_fmt = DXGI_FORMAT_UNKNOWN;
    g_before_valid = false;
}
