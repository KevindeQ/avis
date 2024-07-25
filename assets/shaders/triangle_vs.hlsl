cbuffer Scene : register(b0)
{
    float4x4 matrix_view_projection;
    float4x4 matrix_view;
    float4x4 matrix_projection;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PSInput main(float3 position : POSITION, float4 color : COLOR)
{
    PSInput result;

    result.position = mul(matrix_view_projection, float4(position, 1.0f));
    //result.position = mul(mul(matrix_projection, transpose(matrix_view)), float4(position, 1.0f));
    //result.position.z = 0.25f;
    //result.position.w = 1.0f;
    result.color = color;

    return result;
}