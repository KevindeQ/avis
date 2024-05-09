struct VS_Output
{
    float4 position : SV_Position;
    float2 texture_coords : TEXCOORD0;
};

struct PS_Output
{
    float4 color : SV_Target0;
};

PS_Output PS_Geometry(VS_Output input) : SV_Target
{
    PS_Output output;
    output.color = float4(1.0f, 0.0f, 0.0f, 1.0f);

    return output;
}