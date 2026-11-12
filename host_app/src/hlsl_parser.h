#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "GUI/types.h"

namespace shaderlab {

using Microsoft::WRL::ComPtr;

enum class ShaderDialect {
    Unknown = 0,
    ReShadeFx,
    RawHlsl,
    UnityShader,
    GodotGdShader
};

enum class SystemUniformSemantic {
    None = 0,
    FrameTimeMs,       // ReShade frametime (ms) or seconds depending on source
    FrameTimeSec,      // Seconds delta
    FrameCount,        // uint/int frame counter
    TimerMs,           // ReShade timer (ms)
    TimerSec,          // Elapsed seconds
    BufferSize,        // float2(width, height)
    PixelSize,         // float2(1/width, 1/height)
    BufferWidth,       // float width
    BufferHeight,      // float height
    BufferRcpWidth,    // float 1/width
    BufferRcpHeight,   // float 1/height
    UnityTime,         // float4(t/20, t, t*2, t*3)
    UnitySinTime,      // float4(sin(t/8), sin(t/4), sin(t/2), sin(t))
    UnityScreenParams, // float4(w, h, 1 + 1/w, 1 + 1/h)
    TexelSize          // float4(1/w, 1/h, w, h)
};

struct ReflectedUniformBinding {
    std::string var_name;
    int ui_uniform_index = -1; // index into ShaderLabEffect::uniforms if UI-backed
    SystemUniformSemantic system_semantic = SystemUniformSemantic::None;
    uint32_t byte_offset = 0;
    uint32_t byte_size = 0;
    bool is_int = false;
    bool is_bool = false;
    bool is_uint = false;
};

struct ReflectedTextureSlot {
    uint32_t bind_point = 0;
    std::string hlsl_name;
    enum class SourceKind {
        MainInput = 0,
        DepthBuffer = 1,
        CustomRenderTarget = 2
    } kind = SourceKind::MainInput;
    int custom_rt_index = -1;
};

struct ReflectedSamplerSlot {
    uint32_t bind_point = 0;
    std::string hlsl_name;
    int filter_mode = 0; // 0 = linear, 1 = point
    int address_u = 0;   // 0 = clamp, 1 = wrap, 2 = mirror, 3 = border
    int address_v = 0;
};

struct CustomRenderTargetSpec {
    std::string name;
    uint32_t explicit_width = 0;
    uint32_t explicit_height = 0;
    float width_scale = 1.0f;
    float height_scale = 1.0f;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t mip_levels = 1;
};

struct CustomRenderTarget {
    CustomRenderTargetSpec spec;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t cur_width = 0;
    uint32_t cur_height = 0;
};

struct ReflectedCBuffer {
    uint32_t bind_point = 0;
    uint32_t byte_size = 0;
    ComPtr<ID3D11Buffer> buffer;
    std::vector<ReflectedUniformBinding> bindings;
};

struct CompiledShaderPass {
    std::string pass_name;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    std::vector<ReflectedCBuffer> cbuffers;
    std::vector<ReflectedTextureSlot> texture_slots;
    std::vector<ReflectedSamplerSlot> sampler_slots;
    std::vector<int> output_rt_indices; // empty means renders to active ping-pong output
    bool writes_to_main = true;
    bool ps_outputs_alpha = true;
    bool clear_rt = false;
    bool srgb_write = false;
};

struct CompiledEffectRuntime {
    std::string effect_name;
    std::string file_path;
    std::string engine_type;
    ShaderDialect dialect = ShaderDialect::Unknown;
    std::vector<CustomRenderTarget> custom_rts;
    std::vector<CompiledShaderPass> passes;
    std::vector<std::string> warnings;
    bool valid = false;

    bool ensure_custom_rts(ID3D11Device *device, uint32_t main_w, uint32_t main_h);
};

class UniversalHlslParser {
public:
    static ShaderDialect detect_dialect(const std::filesystem::path &path, const std::string &source);

    static bool parse_and_compile_shader_file(
        ID3D11Device *device,
        const std::filesystem::path &path,
        uint32_t initial_width,
        uint32_t initial_height,
        gui::ShaderLabEffect &out_effect,
        CompiledEffectRuntime &out_runtime,
        std::string &out_error
    );

    static bool parse_and_compile_source(
        ID3D11Device *device,
        const std::string &source,
        const std::filesystem::path &virtual_path,
        ShaderDialect dialect,
        uint32_t initial_width,
        uint32_t initial_height,
        gui::ShaderLabEffect &out_effect,
        CompiledEffectRuntime &out_runtime,
        std::string &out_error
    );
};

} // namespace shaderlab
