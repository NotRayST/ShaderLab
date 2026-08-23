// fullscreen triangle, no vertex buffer needed (SV_VertexID does the work)
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
