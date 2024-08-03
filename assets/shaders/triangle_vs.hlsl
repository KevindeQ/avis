cbuffer Scene : register(b0)
{
    float4x4 matrix_world_view_projection;
};

struct PSInput
{
    float4 position : SV_POSITION;
    uint3 color : COLOR;
};

PSInput main(float3 position : POSITION, uint3 color : COLOR)
{
    PSInput result;

    result.position = mul(matrix_world_view_projection, float4(position, 1.0f));
    result.color = color;

    return result;
}