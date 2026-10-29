// RHI tests (docs/tasks/06-rhi.md §6.8, §6.10): a triangle that covers the viewport, filled with a root-constant color.
// Root signature: 4 root constants at b0, pixel visibility.

struct draw_constants
{
    float4 color;
};

ConstantBuffer<draw_constants> constants : register(b0);

float4 vertex_main(uint vertex : SV_VertexID) : SV_Position
{
    // (-1, 1), (3, 1), (-1, -3): one triangle over the whole viewport
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    return float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 pixel_main() : SV_Target
{
    return constants.color;
}
