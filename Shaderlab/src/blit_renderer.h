#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class BlitRenderer {
public:
    BlitRenderer();
    ~BlitRenderer();

    bool initialize(ID3D11Device *device);
    void shutdown();
    void render(ID3D11DeviceContext *context, ID3D11ShaderResourceView *srv);
    void render_blend(ID3D11DeviceContext *context, ID3D11ShaderResourceView *srv);

private:
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11BlendState> m_blend_state;
};
