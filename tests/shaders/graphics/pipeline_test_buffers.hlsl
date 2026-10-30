// @avis-shader entry=main stage=cs
//
// Graphics tests (docs/tasks/08-graphics-shaders-and-pipelines.md §8.6-8.7): a compute shader the standard signature
// can't run, because it binds buffers by register. Needs the second test signature: constants at b0, a root SRV at t0,
// a root UAV at u0 and a static sampler at s0.
// Bindings: b0 (16 bytes); t0 (shader resource), u0 (unordered access); s0.

struct copy_constants
{
    uint count;
    uint texture_index; // bindless SRV index of a 2D texture
    float2 uv;
};

ConstantBuffer<copy_constants> constants : register(b0);
StructuredBuffer<uint> source : register(t0);
RWStructuredBuffer<uint> destination : register(u0);
SamplerState point_clamp : register(s0);

[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= constants.count)
    {
        return;
    }
    Texture2D<float4> image = ResourceDescriptorHeap[constants.texture_index];
    const float4 texel = image.SampleLevel(point_clamp, constants.uv, 0);
    destination[thread.x] = source[thread.x] + (uint)(texel.r * 255.0);
}
