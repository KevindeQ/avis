// @avis-shader entry=main stage=cs
//
// One mip of a texture's mip chain (docs/design/11-renderer.md §3.6, docs/tasks/11-renderer.md §11.4): a 2 × 2 box
// filter from mip `source_mip` into the next one. The renderer runs it once per mip on an RGBA8_UNORM scratch copy
// of a decoded PNG or JPEG texture; sRGB textures can't have UAVs, so their values are decoded to linear before
// averaging and encoded again after, as the hardware's sRGB views would. Alpha averages linearly.
//
// Odd source sizes clamp at the last row and column, so the box covers 2 × 2 texels everywhere.

#include "bindless.hlsli"
#include "packing.hlsli"

struct mip_constants
{
    uint source; // bindless SRV of the scratch texture (every mip)
    uint destination; // bindless UAV of mip source_mip + 1
    uint source_mip;
    uint srgb; // 1: the texels are sRGB-encoded
    uint2 source_size; // texels of mip source_mip
    uint2 destination_size;
};

AVIS_DRAW_CONSTANTS(mip_constants, constants);

float4 load_linear(Texture2D<float4> source, int2 texel)
{
    const int2 last = int2(constants.source_size) - 1;
    float4 value = source.Load(int3(min(texel, last), constants.source_mip));
    if (constants.srgb != 0)
    {
        value.rgb = srgb_to_linear(value.rgb);
    }
    return value;
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= constants.destination_size))
    {
        return;
    }
    Texture2D<float4> source = ResourceDescriptorHeap[constants.source];
    RWTexture2D<float4> destination = ResourceDescriptorHeap[constants.destination];

    const int2 base = int2(thread.xy) * 2;
    float4 sum = load_linear(source, base);
    sum += load_linear(source, base + int2(1, 0));
    sum += load_linear(source, base + int2(0, 1));
    sum += load_linear(source, base + int2(1, 1));
    float4 average = sum * 0.25;
    if (constants.srgb != 0)
    {
        average.rgb = linear_to_srgb(average.rgb);
    }
    destination[thread.xy] = average;
}
