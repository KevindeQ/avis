struct VS_Output
{
    float4 position : SV_Position;
    float2 texture_coords : TEXCOORD0;
};

// See https://imgeself.github.io/posts/2020-06-19-graphics-study-rdr2/ for more info on g-buffer layout
struct PS_Output
{
    float4 albedo : SV_Target0;
    float2 normal : SV_Target1;
    uint4 material : SV_Target2;
};

Texture2D depth_texture : register(t0);
SamplerState depth_sampler : register(s0);

PS_Output PS_Geometry(VS_Output input) : SV_Target
{
    PS_Output output;
    return output;
}