// @avis-shader entry=main stage=cs
//
// The placeholder for lighting and tonemapping (docs/tasks/11-renderer.md §11.10, until 11.11 and 11.16): ldr_color is
// the G-buffer's base colour, sRGB-encoded, where something was drawn, and the background colour elsewhere. It keeps
// the frame complete and visible while the passes after the G-buffer don't exist yet.

#include "bindless.hlsli"
#include "packing.hlsli"

struct preview_constants
{
    float4 background; // sRGB-encoded, written as it is
    uint albedo; // SRV of gbuffer_albedo (RGBA8_UNORM_SRGB: reads linear)
    uint linear_depth; // SRV, R32F; +infinity where nothing was drawn
    uint ldr_color; // UAV, RGBA8_UNORM
    uint padding0;
    uint2 size; // pixels
};

AVIS_DRAW_CONSTANTS(preview_constants, constants);

[numthreads(8, 8, 1)] void main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= constants.size))
    {
        return;
    }
    Texture2D<float4> albedo = ResourceDescriptorHeap[constants.albedo];
    Texture2D<float> linear_depth = ResourceDescriptorHeap[constants.linear_depth];
    RWTexture2D<float4> ldr_color = ResourceDescriptorHeap[constants.ldr_color];

    const int3 texel = int3(thread.xy, 0);
    float4 color = constants.background;
    if (!isinf(linear_depth.Load(texel)))
    {
        color = float4(linear_to_srgb(albedo.Load(texel).rgb), 1.0);
    }
    ldr_color[thread.xy] = color;
}
