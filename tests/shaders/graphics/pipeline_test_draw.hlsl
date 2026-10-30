// @avis-shader entry=vertex_main stage=vs  entry=pixel_main stage=ps  permutations=TINTED
//
// Graphics tests (docs/tasks/08-graphics-shaders-and-pipelines.md §8.6-8.7): a full-screen triangle on the standard
// signature (graphics::binding_layout). The pixel shader samples a heap texture with a static sampler.
// Bindings: vertex b0 (16 bytes); pixel b1 (32 bytes), s1.

struct draw_constants
{
    float scale;
    float2 offset;
    float depth;
};

struct pass_constants
{
    float4 tint;
    uint texture_index; // bindless SRV index of a 2D texture
    uint3 unused;
};

ConstantBuffer<draw_constants> draw : register(b0);
ConstantBuffer<pass_constants> pass : register(b1);
SamplerState linear_clamp : register(s1);

struct vertex_output
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

vertex_output vertex_main(uint vertex_id : SV_VertexID)
{
    const float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    vertex_output output;
    output.position = float4((uv * 2.0 - 1.0) * draw.scale + draw.offset, draw.depth, 1.0);
    output.uv = uv;
    return output;
}

float4 pixel_main(vertex_output input) : SV_Target
{
    Texture2D<float4> image = ResourceDescriptorHeap[pass.texture_index];
    float4 color = image.Sample(linear_clamp, input.uv);
#ifdef TINTED
    color *= pass.tint;
#endif
    return color;
}
