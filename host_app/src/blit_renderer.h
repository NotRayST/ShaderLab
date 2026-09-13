#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include "viewport_controller.h"

using Microsoft::WRL::ComPtr;

class BlitRenderer {
public:
    BlitRenderer();
    ~BlitRenderer();

    bool initialize(ID3D11Device *device);
    void shutdown();

    // standard blit: fullscreen triangle, point sample, identity
    void render(ID3D11DeviceContext *context, ID3D11ShaderResourceView *srv);

    // alpha blend blit, used for HUD overlays
    void render_blend(ID3D11DeviceContext *context, ID3D11ShaderResourceView *srv);

    // transformed blit: angle, zoom, pan, aspect-fit / canvas mappings
    void render_transformed(
        ID3D11DeviceContext *context,
        ID3D11ShaderResourceView *srv,
        const ViewportTransform &xform,
        float canvas_w,
        float canvas_h,
        float img_w,
        float img_h
    );

    // depth involution blit: transforms linear depth z -> hyperbolic depth d(z) = z*F / (1 + z*(F-1))
    void render_depth(ID3D11DeviceContext *context, ID3D11ShaderResourceView *depth_srv, float far_plane);

    void render_depth_transformed(
        ID3D11DeviceContext *context,
        ID3D11ShaderResourceView *depth_srv,
        const ViewportTransform &xform,
        float canvas_w,
        float canvas_h,
        float img_w,
        float img_h,
        float far_plane,
        bool is_preview = false
    );

    // reshade depth preview blit for human eyes (depth peek), displays depth linearized identically to DisplayDepth.fx
    void render_depth_preview(
        ID3D11DeviceContext *context,
        ID3D11ShaderResourceView *depth_srv,
        const ViewportTransform &xform,
        float canvas_w,
        float canvas_h,
        float img_w,
        float img_h,
        float far_plane
    );

private:
    struct alignas(16) ConstantBufferData {
        float xform_a[4]; // cos_angle, sin_angle, zoom, is_transformed
        float xform_b[4]; // pan_x, pan_y, canvas_w, canvas_h
        float xform_c[4]; // img_w, img_h, use_linear, far_plane
        float xform_d[4]; // is_preview, unused, unused, unused
    };

    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11PixelShader> m_depth_ps;
    ComPtr<ID3D11SamplerState> m_point_sampler;
    ComPtr<ID3D11SamplerState> m_linear_sampler;
    ComPtr<ID3D11BlendState> m_blend_state;
    ComPtr<ID3D11Buffer> m_cbuffer;
};
