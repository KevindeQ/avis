// @avis-shader entry=main stage=cs
//
// The selection outline (docs/design/11-renderer.md §10.5, docs/tasks/11-renderer.md §11.17), one pixel per thread in
// 8 × 8 groups, after the tonemap and before the overlays: a pixel outside selection_mask within
// selection_outline_radius pixels of a pixel inside it (a disc) becomes the selection colour in ldr_color. Other pixels
// are left as they are: the pass writes ldr_color without reading it.

#include "bindless.hlsli"
#include "shared_types.hlsli"

AVIS_PASS_CONSTANTS(selection_outline_constants, constants);

bool selected(Texture2D<float> mask, int2 pixel)
{
    if (any(pixel < 0) || any(pixel >= int2(constants.size)))
    {
        return false;
    }
    return mask.Load(int3(pixel, 0)) > 0.5;
}

[numthreads(8, 8, 1)] void main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= constants.size))
    {
        return;
    }
    Texture2D<float> mask = ResourceDescriptorHeap[constants.selection_mask];
    const int2 pixel = int2(thread.xy);
    if (selected(mask, pixel))
    {
        return;
    }
    const int radius = int(selection_outline_radius);
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            if (x * x + y * y <= radius * radius && selected(mask, pixel + int2(x, y)))
            {
                RWTexture2D<float4> ldr_color = ResourceDescriptorHeap[constants.ldr_color];
                ldr_color[thread.xy] = constants.color;
                return;
            }
        }
    }
}
