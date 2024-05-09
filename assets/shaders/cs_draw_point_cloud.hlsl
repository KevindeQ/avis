cbuffer scene
{
    float4x4 matrix_wvp;
};

cbuffer viewport
{
    float2 screen_dimensions;
};

struct pc_point
{
    float3 position_xyz;
};

StructuredBuffer<pc_point> input_point_cloud;
RWTexture2D<float4> output_buffer;
RWTexture2D<float> depth_buffer;

[numthreads(64, 1, 1)]
void main(uint group_id : SV_GroupIndex)
{
    const float3 color_gray = float3(0.5f, 0.5f, 0.5f);
    
    const float3 position_point_xyz = input_point_cloud[group_id].position_xyz;
    const float4 position_point_world = float4(position_point_xyz, 1.0f);
    
    const float4 position_point_clip = mul(matrix_wvp, position_point_world);
    const float3 position_point_ndc = position_point_clip.xyz / position_point_clip.w;
    const float2 position_point_screen = (position_point_ndc.xy + 1) * 0.5 * screen_dimensions;
    
    depth_buffer[position_point_screen] = position_point_ndc.z;
    output_buffer[position_point_screen] = float4(color_gray, 1.0f);
}