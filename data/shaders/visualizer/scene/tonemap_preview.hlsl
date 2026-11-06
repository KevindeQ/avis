// @avis-shader entry=main stage=cs
//
// The placeholder tonemap (docs/tasks/11-renderer.md §11.11, until 11.16 brings exposure and the ACES and AgX curves):
// ldr_color is hdr_color, which is already pre-exposed, through the Reinhard curve x / (1 + x) per channel, then
// sRGB-encoded. It keeps the frame complete and visible while the real tonemap doesn't exist yet.

#include "bindless.hlsli"
#include "packing.hlsli"

struct preview_constants
{
    uint hdr_color; // SRV, RGBA16F
    uint ldr_color; // UAV, RGBA8_UNORM
    uint2 size; // pixels
};

AVIS_DRAW_CONSTANTS(preview_constants, constants);

[numthreads(8, 8, 1)] void main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= constants.size))
    {
        return;
    }
    Texture2D<float4> hdr_color = ResourceDescriptorHeap[constants.hdr_color];
    RWTexture2D<float4> ldr_color = ResourceDescriptorHeap[constants.ldr_color];

    const float3 color = max(hdr_color.Load(int3(thread.xy, 0)).rgb, 0.0);
    ldr_color[thread.xy] = float4(linear_to_srgb(color / (1.0 + color)), 1.0);
}
