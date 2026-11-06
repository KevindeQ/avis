// @avis-shader entry=main stage=cs
//
// One mip of depth_mips (docs/design/11-renderer.md §2.3 pass 2, §6.3, docs/tasks/11-renderer.md §11.10): the min (R)
// and max (G) of linear depth over the texels of the mip before that each texel covers. A destination texel covers the
// source's 2 × 2 texels, and on the last column or row of an odd-sized source also the third one, so every source texel
// counts and the reduction stays conservative for the cluster and tile tests.

#include "bindless.hlsli"

struct depth_mip_constants
{
    uint source; // SRV of depth_mips (every mip)
    uint destination; // UAV of mip source_mip + 1
    uint source_mip;
    uint padding0;
    uint2 source_size; // texels of mip source_mip
    uint2 destination_size;
};

AVIS_DRAW_CONSTANTS(depth_mip_constants, constants);

[numthreads(8, 8, 1)] void main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= constants.destination_size))
    {
        return;
    }
    Texture2D<float2> source = ResourceDescriptorHeap[constants.source];
    RWTexture2D<float2> destination = ResourceDescriptorHeap[constants.destination];

    const uint2 first = thread.xy * 2;
    uint2 last = min(first + 1, constants.source_size - 1);
    // An odd source's last texel belongs to the last destination texel
    if (thread.x == constants.destination_size.x - 1)
    {
        last.x = constants.source_size.x - 1;
    }
    if (thread.y == constants.destination_size.y - 1)
    {
        last.y = constants.source_size.y - 1;
    }

    float2 reduced = source.Load(int3(first, constants.source_mip));
    for (uint y = first.y; y <= last.y; ++y)
    {
        for (uint x = first.x; x <= last.x; ++x)
        {
            const float2 value = source.Load(int3(x, y, constants.source_mip));
            reduced = float2(min(reduced.x, value.x), max(reduced.y, value.y));
        }
    }
    destination[thread.xy] = reduced;
}
